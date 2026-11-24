use heic_decoder::{ChromaUpsampling, ColorInfo, DecodedImage, RgbOptions};
use rayon::prelude::*;
use std::ptr;
use std::slice;
use std::sync::OnceLock;

pub const MODE_32F_LINEAR_HDR: u32 = 0;
pub const MODE_16U_TONEMAPPED: u32 = 1;
pub const MODE_16U_RAW_SIGNAL: u32 = 2;

#[repr(C)]
pub struct SonyHifProbe {
    pub width: u32,
    pub height: u32,
    pub bit_depth: u32,
    pub color_primaries: u16,
    pub transfer_characteristics: u16,
    pub is_hdr: u32,
}

#[repr(C)]
pub struct SonyHifDecoded {
    pub width: u32,
    pub height: u32,
    pub planes: u32,
    pub depth: u32, // 16 or 32
    pub has_alpha: u32,
    pub pixel_ptr: *mut u8,
    pub pixel_len: usize,
    pub icc_ptr: *mut u8,
    pub icc_len: usize,
    pub exif_ptr: *mut u8,
    pub exif_len: usize,
    pub xmp_ptr: *mut u8,
    pub xmp_len: usize,
}

#[inline]
fn hlg_inv_oetf(v: f32) -> f32 {
    const A: f32 = 0.17883277;
    const B: f32 = 1.0 - 4.0 * A;
    const C: f32 = 0.55991073;
    if v <= 0.5 {
        (v * v) / 3.0
    } else {
        (((v - C) / A).exp() + B) / 12.0
    }
}

#[inline]
fn pq_eotf_sdr_norm(v: f32) -> f32 {
    const M1: f32 = 2610.0 / 16384.0;
    const M2: f32 = 2523.0 / 4096.0 * 128.0;
    const C1: f32 = 3424.0 / 4096.0;
    const C2: f32 = 2413.0 / 4096.0 * 32.0;
    const C3: f32 = 2392.0 / 4096.0 * 32.0;
    let vp = v.clamp(0.0, 1.0).powf(1.0 / M2);
    let num = (vp - C1).max(0.0);
    let den = (C2 - C3 * vp).max(1e-7);
    let y = (num / den).powf(1.0 / M1);
    y * (10000.0 / 203.0)
}

#[inline]
fn srgb_to_linear(v: f32) -> f32 {
    let v = v.clamp(0.0, 1.0);
    if v <= 0.04045 {
        v / 12.92
    } else {
        ((v + 0.055) / 1.055).powf(2.4)
    }
}

#[inline]
fn linear_to_srgb(v: f32) -> f32 {
    let v = v.clamp(0.0, 1.0);
    if v <= 0.0031308 {
        12.92 * v
    } else {
        1.055 * v.powf(1.0 / 2.4) - 0.055
    }
}

fn hlg_lut() -> &'static [f32] {
    static LUT: OnceLock<Vec<f32>> = OnceLock::new();
    LUT.get_or_init(|| {
        (0..=65535u32)
            .map(|i| hlg_inv_oetf(i as f32 / 65535.0))
            .collect()
    })
}

fn srgb_sqrt_lut_ps16() -> &'static [f32] {
    static LUT: OnceLock<Vec<f32>> = OnceLock::new();
    LUT.get_or_init(|| {
        const N: usize = 8192;
        (0..=N)
            .map(|i| {
                let s = i as f32 / N as f32;
                let lin = s * s;
                linear_to_srgb(lin) * 32768.0
            })
            .collect()
    })
}

#[inline(always)]
fn linear_to_srgb_ps16_fast(lut: &[f32], v: f32) -> u16 {
    let s = v.clamp(0.0, 1.0).sqrt();
    let fidx = s * 8192.0;
    let idx = (fidx as usize).min(8191);
    let frac = fidx - idx as f32;
    let val = lut[idx] + (lut[idx + 1] - lut[idx]) * frac;
    val.round().clamp(0.0, 32768.0) as u16
}

// Build ICC v4.0 RGB matrix+TRC profile (D50 PCS, Bradford D65->D50, desc/text/para)
// When `is_linear` is true, TRC uses parametricType 0 gamma = 1.0 (required for PS 32-bit RGB).
fn build_icc_v4(
    desc: &str,
    r_xyz: [i32; 3],
    g_xyz: [i32; 3],
    b_xyz: [i32; 3],
    is_linear: bool,
) -> Vec<u8> {
    fn push_be32(buf: &mut Vec<u8>, v: u32) {
        buf.extend_from_slice(&v.to_be_bytes());
    }
    fn push_be32_i(buf: &mut Vec<u8>, v: i32) {
        buf.extend_from_slice(&v.to_be_bytes());
    }
    fn push_be16(buf: &mut Vec<u8>, v: u16) {
        buf.extend_from_slice(&v.to_be_bytes());
    }
    fn pad4(buf: &mut Vec<u8>) {
        while buf.len() % 4 != 0 {
            buf.push(0);
        }
    }
    fn make_xyz_tag(xyz: [i32; 3]) -> Vec<u8> {
        let mut b = Vec::with_capacity(20);
        b.extend_from_slice(b"XYZ ");
        push_be32(&mut b, 0);
        push_be32_i(&mut b, xyz[0]);
        push_be32_i(&mut b, xyz[1]);
        push_be32_i(&mut b, xyz[2]);
        b
    }
    fn make_chad_tag() -> Vec<u8> {
        const CHAD: [i32; 9] = [
            0x00010c42, 0x000005de, -0x00000cdb,
            0x00000793, 0x0000fd90, -0x0000045f,
            -0x0000025e, 0x000003dc, 0x0000c06e,
        ];
        let mut b = Vec::with_capacity(44);
        b.extend_from_slice(b"sf32");
        push_be32(&mut b, 0);
        for v in CHAD {
            push_be32_i(&mut b, v);
        }
        b
    }
    fn make_para_tag(is_linear: bool) -> Vec<u8> {
        let mut b = Vec::with_capacity(32);
        b.extend_from_slice(b"para");
        push_be32(&mut b, 0);
        if is_linear {
            push_be16(&mut b, 0); // functionType = 0 (Y = X^gamma)
            push_be16(&mut b, 0);
            push_be32_i(&mut b, 0x00010000); // gamma = 1.0
        } else {
            push_be16(&mut b, 3); // functionType = 3 (sRGB IEC 61966-2.1)
            push_be16(&mut b, 0);
            push_be32_i(&mut b, 0x00026666); // g = 2.4
            push_be32_i(&mut b, 0x0000f2a7); // a = 1 / 1.055
            push_be32_i(&mut b, 0x00000d59); // b = 0.055 / 1.055
            push_be32_i(&mut b, 0x000013d0); // c = 1 / 12.92
            push_be32_i(&mut b, 0x00000a5b); // d = 0.04045
        }
        b
    }
    fn make_desc_tag(s: &str) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(b"desc");
        push_be32(&mut b, 0);
        let ascii_len = (s.len() + 1) as u32;
        push_be32(&mut b, ascii_len);
        b.extend_from_slice(s.as_bytes());
        b.push(0);
        b.resize(b.len() + 78, 0);
        pad4(&mut b);
        b
    }
    fn make_text_tag(s: &str) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(b"text");
        push_be32(&mut b, 0);
        b.extend_from_slice(s.as_bytes());
        b.push(0);
        pad4(&mut b);
        b
    }

    const D50_ICC: [i32; 3] = [0x0000f6d6, 0x00010000, 0x0000d32d];
    let para = make_para_tag(is_linear);
    let tag_defs: [(&[u8; 4], Vec<u8>); 7] = [
        (b"desc", make_desc_tag(desc)),
        (b"cprt", make_text_tag("Public Domain")),
        (b"wtpt", make_xyz_tag(D50_ICC)),
        (b"chad", make_chad_tag()),
        (b"rXYZ", make_xyz_tag(r_xyz)),
        (b"gXYZ", make_xyz_tag(g_xyz)),
        (b"bXYZ", make_xyz_tag(b_xyz)),
    ];

    let tag_count: u32 = 10;
    let mut cur_off = 128u32 + 4 + tag_count * 12;
    let mut entries: Vec<([u8; 4], u32, u32)> = Vec::with_capacity(10);
    let mut blobs = Vec::new();

    for (sig, blob) in tag_defs {
        let sz = blob.len() as u32;
        entries.push((*sig, cur_off, sz));
        blobs.extend_from_slice(&blob);
        cur_off += sz;
    }

    let para_off = cur_off;
    let para_sz = para.len() as u32;
    blobs.extend_from_slice(&para);
    cur_off += para_sz;

    entries.push((*b"rTRC", para_off, para_sz));
    entries.push((*b"gTRC", para_off, para_sz));
    entries.push((*b"bTRC", para_off, para_sz));

    let total_size = cur_off;
    let mut icc = vec![0u8; 128];
    icc[0..4].copy_from_slice(&total_size.to_be_bytes());
    icc[8..12].copy_from_slice(&0x04000000u32.to_be_bytes());
    icc[12..16].copy_from_slice(b"mntr");
    icc[16..20].copy_from_slice(b"RGB ");
    icc[20..24].copy_from_slice(b"XYZ ");
    icc[24..26].copy_from_slice(&2026u16.to_be_bytes());
    icc[26..28].copy_from_slice(&1u16.to_be_bytes());
    icc[28..30].copy_from_slice(&1u16.to_be_bytes());
    icc[36..40].copy_from_slice(b"acsp");
    icc[68..72].copy_from_slice(&D50_ICC[0].to_be_bytes());
    icc[72..76].copy_from_slice(&D50_ICC[1].to_be_bytes());
    icc[76..80].copy_from_slice(&D50_ICC[2].to_be_bytes());
    icc[80..84].copy_from_slice(b"appl");

    push_be32(&mut icc, tag_count);
    for (sig, off, sz) in entries {
        icc.extend_from_slice(&sig);
        push_be32(&mut icc, off);
        push_be32(&mut icc, sz);
    }
    icc.extend_from_slice(&blobs);
    icc
}

fn bt2020_linear_icc() -> &'static [u8] {
    static ICC: OnceLock<Vec<u8>> = OnceLock::new();
    ICC.get_or_init(|| {
        build_icc_v4(
            "Rec. 2020 (Linear RGB Profile)",
            [44137, 18287, -127],
            [10857, 44259, 1965],
            [8195, 2989, 52223],
            true,
        )
    })
}

fn bt2020_srgb_icc() -> &'static [u8] {
    static ICC: OnceLock<Vec<u8>> = OnceLock::new();
    ICC.get_or_init(|| {
        build_icc_v4(
            "ITU-R BT.2020 (sRGB TRC)",
            [44137, 18287, -127],
            [10857, 44259, 1965],
            [8195, 2989, 52223],
            false,
        )
    })
}

fn display_p3_linear_icc() -> &'static [u8] {
    static ICC: OnceLock<Vec<u8>> = OnceLock::new();
    ICC.get_or_init(|| {
        build_icc_v4(
            "Display P3 (Linear RGB Profile)",
            [33759, 15807, -69],
            [19135, 45367, 2745],
            [10296, 4363, 51385],
            true,
        )
    })
}

fn display_p3_srgb_icc() -> &'static [u8] {
    static ICC: OnceLock<Vec<u8>> = OnceLock::new();
    ICC.get_or_init(|| {
        build_icc_v4(
            "Display P3",
            [33759, 15807, -69],
            [19135, 45367, 2745],
            [10296, 4363, 51385],
            false,
        )
    })
}

fn srgb_linear_icc() -> &'static [u8] {
    static ICC: OnceLock<Vec<u8>> = OnceLock::new();
    ICC.get_or_init(|| {
        // Bradford D65->D50 sRGB primaries
        build_icc_v4(
            "sRGB IEC61966-2.1 (Linear RGB Profile)",
            [0x6fa2, 0x38f5, 0x0390],
            [0x6299, 0xb785, 0x18da],
            [0x24a0, 0x0f84, 0xb6cf],
            true,
        )
    })
}

fn srgb_icc() -> &'static [u8] {
    static ICC: OnceLock<Vec<u8>> = OnceLock::new();
    ICC.get_or_init(|| {
        build_icc_v4(
            "sRGB IEC61966-2.1",
            [0x6fa2, 0x38f5, 0x0390],
            [0x6299, 0xb785, 0x18da],
            [0x24a0, 0x0f84, 0xb6cf],
            false,
        )
    })
}

fn to_rgba16_with_fallback(
    img: &DecodedImage,
    ci_opt: Option<ColorInfo>,
) -> Option<heic_decoder::RgbaImage<u16>> {
    img.to_rgba16()
        .or_else(|_| {
            let prev = ci_opt.unwrap_or(ColorInfo {
                color_primaries: 2,
                transfer_characteristics: 2,
                matrix_coefficients: 6,
                full_range: true,
            });
            let fallback = ColorInfo {
                color_primaries: prev.color_primaries,
                transfer_characteristics: prev.transfer_characteristics,
                matrix_coefficients: 6,
                full_range: prev.full_range,
            };
            img.to_rgba16_with_options(&RgbOptions {
                color: Some(fallback),
                upsampling: ChromaUpsampling::Bilinear,
            })
        })
        .ok()
}

// Reset EXIF IFD0 Orientation tag (0x0112) to 1 because pixels are already rotated by heic-decoder.
fn patch_exif_orientation_to_1(exif: &mut [u8]) {
    if exif.len() < 8 {
        return;
    }
    let le = if &exif[0..4] == b"II*\0" {
        true
    } else if &exif[0..4] == b"MM\0*" {
        false
    } else {
        return;
    };
    let r16 = |b: &[u8], off: usize| -> u16 {
        if le {
            u16::from_le_bytes([b[off], b[off + 1]])
        } else {
            u16::from_be_bytes([b[off], b[off + 1]])
        }
    };
    let r32 = |b: &[u8], off: usize| -> u32 {
        if le {
            u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
        } else {
            u32::from_be_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
        }
    };
    let ifd_off = r32(exif, 4) as usize;
    if ifd_off + 2 > exif.len() {
        return;
    }
    let count = r16(exif, ifd_off) as usize;
    for i in 0..count {
        let e = ifd_off + 2 + i * 12;
        if e + 12 > exif.len() {
            break;
        }
        let tag = r16(exif, e);
        let typ = r16(exif, e + 2);
        let cnt = r32(exif, e + 4);
        if tag == 0x0112 && typ == 3 && cnt == 1 {
            if le {
                exif[e + 8] = 1;
                exif[e + 9] = 0;
            } else {
                exif[e + 8] = 0;
                exif[e + 9] = 1;
            }
            break;
        }
    }
}

fn scan_probe_boxes(buf: &[u8], info: &mut SonyHifProbe, rot_deg: &mut u32) {
    let mut off = 0usize;
    while off + 8 <= buf.len() {
        let mut sz = u32::from_be_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]]) as u64;
        let tp = &buf[off + 4..off + 8];
        let mut hdr = 8usize;
        if sz == 1 && off + 16 <= buf.len() {
            sz = u64::from_be_bytes([
                buf[off + 8],
                buf[off + 9],
                buf[off + 10],
                buf[off + 11],
                buf[off + 12],
                buf[off + 13],
                buf[off + 14],
                buf[off + 15],
            ]);
            hdr = 16;
        } else if sz == 0 {
            sz = (buf.len() - off) as u64;
        }
        if sz < hdr as u64 || off + (sz as usize) > buf.len() {
            break;
        }
        let end = off + sz as usize;
        let payload = &buf[off + hdr..end];

        if tp == b"meta" && payload.len() >= 4 {
            scan_probe_boxes(&payload[4..], info, rot_deg);
        } else if tp == b"iprp" || tp == b"ipco" {
            scan_probe_boxes(payload, info, rot_deg);
        } else if tp == b"ispe" && payload.len() >= 12 {
            let w = u32::from_be_bytes([payload[4], payload[5], payload[6], payload[7]]);
            let h = u32::from_be_bytes([payload[8], payload[9], payload[10], payload[11]]);
            if w > info.width && h > 0 {
                info.width = w;
                info.height = h;
            }
        } else if tp == b"irot" && !payload.is_empty() {
            *rot_deg = ((payload[0] & 3) as u32) * 90;
        } else if tp == b"hvcC" && payload.len() >= 18 {
            let bd = ((payload[17] & 0x07) as u32) + 8;
            if bd > info.bit_depth {
                info.bit_depth = bd;
            }
        } else if tp == b"colr" && payload.len() >= 11 && &payload[0..4] == b"nclx" {
            info.color_primaries = u16::from_be_bytes([payload[4], payload[5]]);
            info.transfer_characteristics = u16::from_be_bytes([payload[6], payload[7]]);
        }
        off = end;
    }
}

#[no_mangle]
pub unsafe extern "C" fn sony_hif_probe(
    input_ptr: *const u8,
    input_len: usize,
    out_probe: *mut SonyHifProbe,
) -> i32 {
    if input_ptr.is_null() || input_len < 12 || out_probe.is_null() {
        return -1;
    }
    let bytes = slice::from_raw_parts(input_ptr, input_len);
    let mut probe = SonyHifProbe {
        width: 0,
        height: 0,
        bit_depth: 8,
        color_primaries: 1,
        transfer_characteristics: 13,
        is_hdr: 0,
    };
    let mut rot_deg = 0u32;
    // Limit fast header scan to first 256 KB
    let scan_len = bytes.len().min(256 * 1024);
    scan_probe_boxes(&bytes[..scan_len], &mut probe, &mut rot_deg);
    if rot_deg == 90 || rot_deg == 270 {
        std::mem::swap(&mut probe.width, &mut probe.height);
    }
    if probe.transfer_characteristics == 18 || probe.transfer_characteristics == 16 {
        probe.is_hdr = 1;
    }
    *out_probe = probe;
    0
}

fn alloc_c_buf(mut v: Vec<u8>) -> (*mut u8, usize) {
    if v.is_empty() {
        return (ptr::null_mut(), 0);
    }
    v.shrink_to_fit();
    let len = v.len();
    let ptr = v.as_mut_ptr();
    std::mem::forget(v);
    (ptr, len)
}

#[no_mangle]
pub unsafe extern "C" fn sony_hif_decode(
    input_ptr: *const u8,
    input_len: usize,
    mode: u32,
    out: *mut SonyHifDecoded,
) -> i32 {
    if input_ptr.is_null() || input_len == 0 || out.is_null() {
        return -1;
    }
    let bytes = slice::from_raw_parts(input_ptr, input_len);
    let Ok(img) = heic_decoder::decode(bytes) else {
        return -2;
    };

    let ci_opt = img.color_info;
    let cp = ci_opt.map(|c| c.color_primaries).unwrap_or(1);
    let tc = ci_opt.map(|c| c.transfer_characteristics).unwrap_or(13);
    let embedded_icc = img.metadata.icc_profile.clone();

    let Some(rgba16) = to_rgba16_with_fallback(&img, ci_opt) else {
        return -3;
    };

    let w = rgba16.width as u32;
    let h = rgba16.height as u32;
    let has_alpha = img.alpha.is_some();
    let planes: u32 = if has_alpha { 4 } else { 3 };
    let premul = rgba16.premultiplied;
    const INV_65535: f32 = 1.0 / 65535.0;

    let (yr, yg, yb) = if cp == 9 {
        (0.2627_f32, 0.6780_f32, 0.0593_f32)
    } else {
        (0.2126_f32, 0.7152_f32, 0.0722_f32)
    };

    let (depth_out, pixel_bytes, synthesized_icc): (u32, Vec<u8>, Option<&'static [u8]>) =
        match mode {
            MODE_32F_LINEAR_HDR => {
                // Output interleaved f32 RGB or RGBA linear scene light in native gamut.
                // HLG uses ITU-R BT.2408 OOTF (gamma=1.2, 75% diffuse white = 1.0, peak = 4.9261).
                let out_floats = (w as usize) * (h as usize) * (planes as usize);
                let mut out_bytes = vec![0u8; out_floats * 4];
                let hlg_table = if tc == 18 { Some(hlg_lut()) } else { None };
                let row_floats = (w as usize) * (planes as usize);

                out_bytes
                    .par_chunks_exact_mut(row_floats * 4)
                    .zip(rgba16.data.par_chunks_exact((w as usize) * 4))
                    .for_each(|(dst_row, src_row)| {
                        for x in 0..(w as usize) {
                            let src = &src_row[x * 4..x * 4 + 4];
                            let a16 = src[3];
                            let af = a16 as f32 * INV_65535;
                            let no_premul = !(premul && a16 > 0 && a16 < 65535);

                            let (r_norm, g_norm, b_norm) = if no_premul {
                                (
                                    src[0] as f32 * INV_65535,
                                    src[1] as f32 * INV_65535,
                                    src[2] as f32 * INV_65535,
                                )
                            } else {
                                (
                                    (src[0] as f32 * INV_65535 / af).min(1.0),
                                    (src[1] as f32 * INV_65535 / af).min(1.0),
                                    (src[2] as f32 * INV_65535 / af).min(1.0),
                                )
                            };

                            let (lr, lg, lb) = match tc {
                                18 => {
                                    let (sr, sg, sb) = if let (Some(tbl), true) = (hlg_table, no_premul) {
                                        (tbl[src[0] as usize], tbl[src[1] as usize], tbl[src[2] as usize])
                                    } else {
                                        (hlg_inv_oetf(r_norm), hlg_inv_oetf(g_norm), hlg_inv_oetf(b_norm))
                                    };
                                    let y_lum = (yr * sr + yg * sg + yb * sb).max(0.0);
                                    if y_lum > 1e-8 {
                                        // BT.2408 HLG OOTF: 1000/203 * Y^0.2 * RGB_scene
                                        const HLG_SCALE: f32 = 1000.0 / 203.0;
                                        let ootf = HLG_SCALE * y_lum.powf(0.2);
                                        (sr * ootf, sg * ootf, sb * ootf)
                                    } else {
                                        (0.0, 0.0, 0.0)
                                    }
                                }
                                16 => (
                                    pq_eotf_sdr_norm(r_norm),
                                    pq_eotf_sdr_norm(g_norm),
                                    pq_eotf_sdr_norm(b_norm),
                                ),
                                _ => (
                                    srgb_to_linear(r_norm),
                                    srgb_to_linear(g_norm),
                                    srgb_to_linear(b_norm),
                                ),
                            };

                            let base = x * (planes as usize) * 4;
                            dst_row[base..base + 4].copy_from_slice(&lr.to_le_bytes());
                            dst_row[base + 4..base + 8].copy_from_slice(&lg.to_le_bytes());
                            dst_row[base + 8..base + 12].copy_from_slice(&lb.to_le_bytes());
                            if has_alpha {
                                dst_row[base + 12..base + 16].copy_from_slice(&af.to_le_bytes());
                            }
                        }
                    });

                let icc = match cp {
                    9 => Some(bt2020_linear_icc()),
                    12 => Some(display_p3_linear_icc()),
                    _ => Some(srgb_linear_icc()),
                };
                (32, out_bytes, icc)
            }
            MODE_16U_TONEMAPPED => {
                let out_u16s = (w as usize) * (h as usize) * (planes as usize);
                let mut out_bytes = vec![0u8; out_u16s * 2];
                let is_hdr_tc = embedded_icc.is_none() && (tc == 18 || tc == 16);
                let hlg_table = if is_hdr_tc && tc == 18 { Some(hlg_lut()) } else { None };
                let srgb_table = srgb_sqrt_lut_ps16();
                let row_u16s = (w as usize) * (planes as usize);

                out_bytes
                    .par_chunks_exact_mut(row_u16s * 2)
                    .zip(rgba16.data.par_chunks_exact((w as usize) * 4))
                    .for_each(|(dst_row, src_row)| {
                        for x in 0..(w as usize) {
                            let src = &src_row[x * 4..x * 4 + 4];
                            let a16 = src[3];
                            let no_premul = !(premul && a16 > 0 && a16 < 65535);

                            let (r16, g16, b16) = if is_hdr_tc {
                                let (lr, lg, lb) = if tc == 18 {
                                    let (sr, sg, sb) = if let (Some(tbl), true) = (hlg_table, no_premul) {
                                        (tbl[src[0] as usize], tbl[src[1] as usize], tbl[src[2] as usize])
                                    } else {
                                        let af = a16 as f32 * INV_65535;
                                        (
                                            hlg_inv_oetf((src[0] as f32 * INV_65535 / af).min(1.0)),
                                            hlg_inv_oetf((src[1] as f32 * INV_65535 / af).min(1.0)),
                                            hlg_inv_oetf((src[2] as f32 * INV_65535 / af).min(1.0)),
                                        )
                                    };
                                    let y_lum = yr * sr + yg * sg + yb * sb;
                                    if y_lum > 1e-7 {
                                        const GAIN: f32 = 4.8;
                                        let l = y_lum * GAIN;
                                        let l_out = l * (1.0 + l / (GAIN * GAIN)) / (1.0 + l);
                                        let s = l_out / y_lum;
                                        (sr * s, sg * s, sb * s)
                                    } else {
                                        (0.0, 0.0, 0.0)
                                    }
                                } else {
                                    let (mut r, mut g, mut b) = (
                                        src[0] as f32 * INV_65535,
                                        src[1] as f32 * INV_65535,
                                        src[2] as f32 * INV_65535,
                                    );
                                    if !no_premul {
                                        let af = a16 as f32 * INV_65535;
                                        r = (r / af).min(1.0);
                                        g = (g / af).min(1.0);
                                        b = (b / af).min(1.0);
                                    }
                                    let (sr, sg, sb) = (
                                        pq_eotf_sdr_norm(r),
                                        pq_eotf_sdr_norm(g),
                                        pq_eotf_sdr_norm(b),
                                    );
                                    let y_lum = yr * sr + yg * sg + yb * sb;
                                    if y_lum > 1e-7 {
                                        const WHITE: f32 = 5.0;
                                        let l_out = y_lum * (1.0 + y_lum / (WHITE * WHITE)) / (1.0 + y_lum);
                                        let s = l_out / y_lum;
                                        (sr * s, sg * s, sb * s)
                                    } else {
                                        (0.0, 0.0, 0.0)
                                    }
                                };
                                (
                                    linear_to_srgb_ps16_fast(srgb_table, lr),
                                    linear_to_srgb_ps16_fast(srgb_table, lg),
                                    linear_to_srgb_ps16_fast(srgb_table, lb),
                                )
                            } else {
                                let (mut r, mut g, mut b) = (src[0], src[1], src[2]);
                                if !no_premul {
                                    let a32 = a16 as u32;
                                    r = (((r as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                                    g = (((g as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                                    b = (((b as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                                }
                                (
                                    (((r as u32) * 32768 + 32767) / 65535) as u16,
                                    (((g as u32) * 32768 + 32767) / 65535) as u16,
                                    (((b as u32) * 32768 + 32767) / 65535) as u16,
                                )
                            };

                            let base = x * (planes as usize) * 2;
                            dst_row[base..base + 2].copy_from_slice(&r16.to_le_bytes());
                            dst_row[base + 2..base + 4].copy_from_slice(&g16.to_le_bytes());
                            dst_row[base + 4..base + 6].copy_from_slice(&b16.to_le_bytes());
                            if has_alpha {
                                let a_ps = (((a16 as u32) * 32768 + 32767) / 65535) as u16;
                                dst_row[base + 6..base + 8].copy_from_slice(&a_ps.to_le_bytes());
                            }
                        }
                    });

                let icc = if embedded_icc.is_some() {
                    None
                } else {
                    match cp {
                        9 => Some(bt2020_srgb_icc()),
                        12 => Some(display_p3_srgb_icc()),
                        _ => Some(srgb_icc()),
                    }
                };
                (16, out_bytes, icc)
            }
            _ => {
                // MODE_16U_RAW_SIGNAL: direct 10/16-bit coded signal scaled to Photoshop 0..=32768
                let out_u16s = (w as usize) * (h as usize) * (planes as usize);
                let mut out_bytes = vec![0u8; out_u16s * 2];
                let row_u16s = (w as usize) * (planes as usize);

                out_bytes
                    .par_chunks_exact_mut(row_u16s * 2)
                    .zip(rgba16.data.par_chunks_exact((w as usize) * 4))
                    .for_each(|(dst_row, src_row)| {
                        for x in 0..(w as usize) {
                            let src = &src_row[x * 4..x * 4 + 4];
                            let a16 = src[3];
                            let (mut r, mut g, mut b) = (src[0], src[1], src[2]);
                            if premul && a16 > 0 && a16 < 65535 {
                                let a32 = a16 as u32;
                                r = (((r as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                                g = (((g as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                                b = (((b as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                            }
                            let r_ps = (((r as u32) * 32768 + 32767) / 65535) as u16;
                            let g_ps = (((g as u32) * 32768 + 32767) / 65535) as u16;
                            let b_ps = (((b as u32) * 32768 + 32767) / 65535) as u16;
                            let base = x * (planes as usize) * 2;
                            dst_row[base..base + 2].copy_from_slice(&r_ps.to_le_bytes());
                            dst_row[base + 2..base + 4].copy_from_slice(&g_ps.to_le_bytes());
                            dst_row[base + 4..base + 6].copy_from_slice(&b_ps.to_le_bytes());
                            if has_alpha {
                                let a_ps = (((a16 as u32) * 32768 + 32767) / 65535) as u16;
                                dst_row[base + 6..base + 8].copy_from_slice(&a_ps.to_le_bytes());
                            }
                        }
                    });

                let icc = if embedded_icc.is_some() {
                    None
                } else {
                    match cp {
                        9 => Some(bt2020_srgb_icc()),
                        12 => Some(display_p3_srgb_icc()),
                        _ => Some(srgb_icc()),
                    }
                };
                (16, out_bytes, icc)
            }
        };

    // Note: in 32-bit float linear mode, do NOT reuse an embedded gamma ICC; use the linear ICC.
    let icc_vec = if mode == MODE_32F_LINEAR_HDR {
        synthesized_icc.map(|s| s.to_vec())
    } else {
        embedded_icc.or_else(|| synthesized_icc.map(|s| s.to_vec()))
    };

    let exif_vec = img.metadata.exif.clone().map(|mut e| {
        patch_exif_orientation_to_1(&mut e);
        e
    });
    let xmp_vec = img.metadata.xmp.clone();

    let (pixel_ptr, pixel_len) = alloc_c_buf(pixel_bytes);
    let (icc_ptr, icc_len) = icc_vec.map_or((ptr::null_mut(), 0), alloc_c_buf);
    let (exif_ptr, exif_len) = exif_vec.map_or((ptr::null_mut(), 0), alloc_c_buf);
    let (xmp_ptr, xmp_len) = xmp_vec.map_or((ptr::null_mut(), 0), alloc_c_buf);

    *out = SonyHifDecoded {
        width: w,
        height: h,
        planes,
        depth: depth_out,
        has_alpha: if has_alpha { 1 } else { 0 },
        pixel_ptr,
        pixel_len,
        icc_ptr,
        icc_len,
        exif_ptr,
        exif_len,
        xmp_ptr,
        xmp_len,
    };

    0
}

unsafe fn free_c_buf(p: &mut *mut u8, len: &mut usize) {
    if !(*p).is_null() && *len > 0 {
        let _ = Vec::from_raw_parts(*p, *len, *len);
    }
    *p = ptr::null_mut();
    *len = 0;
}

#[no_mangle]
pub unsafe extern "C" fn sony_hif_free(decoded: *mut SonyHifDecoded) {
    if decoded.is_null() {
        return;
    }
    let d = &mut *decoded;
    free_c_buf(&mut d.pixel_ptr, &mut d.pixel_len);
    free_c_buf(&mut d.icc_ptr, &mut d.icc_len);
    free_c_buf(&mut d.exif_ptr, &mut d.exif_len);
    free_c_buf(&mut d.xmp_ptr, &mut d.xmp_len);
}
