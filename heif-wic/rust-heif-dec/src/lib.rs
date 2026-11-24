use heic_decoder::{ChromaUpsampling, ColorInfo, DecodedImage, RgbOptions};
use rayon::prelude::*;
use std::ptr;
use std::slice;
use std::sync::OnceLock;

#[repr(C)]
pub struct HeifWicDecoded {
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub bits_per_channel: u32,
    pub bgra_ptr: *mut u8,
    pub bgra_len: usize,
    pub icc_ptr: *mut u8,
    pub icc_len: usize,
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

fn hlg_lut() -> &'static [f32] {
    static LUT: OnceLock<Vec<f32>> = OnceLock::new();
    LUT.get_or_init(|| {
        (0..=65535u32)
            .map(|i| hlg_inv_oetf(i as f32 / 65535.0))
            .collect()
    })
}

const BT2020_TO_DISPLAY_P3: [[f32; 3]; 3] = [
    [1.3435783, -0.2821797, -0.0613986],
    [-0.0652975, 1.0757879, -0.0104905],
    [0.0028218, -0.0195985, 1.0167767],
];

// Zero-mean 4x4 Bayer ordered dithering offsets in [-15/32, +15/32] LSB.
const BAYER_4X4: [[f32; 4]; 4] = [
    [-15.0 / 32.0, 1.0 / 32.0, -11.0 / 32.0, 5.0 / 32.0],
    [9.0 / 32.0, -7.0 / 32.0, 13.0 / 32.0, -3.0 / 32.0],
    [-9.0 / 32.0, 7.0 / 32.0, -13.0 / 32.0, 3.0 / 32.0],
    [15.0 / 32.0, -1.0 / 32.0, 11.0 / 32.0, -5.0 / 32.0],
];

fn srgb_sqrt_lut_255() -> &'static [f32] {
    static LUT: OnceLock<Vec<f32>> = OnceLock::new();
    LUT.get_or_init(|| {
        const N: usize = 8192;
        (0..=N)
            .map(|i| {
                let s = i as f32 / N as f32;
                let lin = s * s;
                linear_to_srgb(lin) * 255.0
            })
            .collect()
    })
}

#[inline(always)]
fn linear_to_srgb_f255_fast(lut: &[f32], v: f32) -> f32 {
    let s = v.clamp(0.0, 1.0).sqrt();
    let fidx = s * 8192.0;
    let idx = (fidx as usize).min(8191);
    let frac = fidx - idx as f32;
    lut[idx] + (lut[idx + 1] - lut[idx]) * frac
}

#[inline(always)]
fn dither_to_u8(val_255: f32, dither: f32) -> u8 {
    (val_255 + dither).round().clamp(0.0, 255.0) as u8
}

#[inline(always)]
fn bt2020_to_display_p3_linear(r: f32, g: f32, b: f32) -> (f32, f32, f32) {
    let m = &BT2020_TO_DISPLAY_P3;
    (
        (m[0][0] * r + m[0][1] * g + m[0][2] * b).clamp(0.0, 1.0),
        (m[1][0] * r + m[1][1] * g + m[1][2] * b).clamp(0.0, 1.0),
        (m[2][0] * r + m[2][1] * g + m[2][2] * b).clamp(0.0, 1.0),
    )
}

// Build a standard ICC v4.0 RGB matrix+TRC profile (D50 PCS, Bradford D65->D50, mluc, sRGB para).
fn build_icc_v4(desc: &str, r_xyz: [i32; 3], g_xyz: [i32; 3], b_xyz: [i32; 3]) -> Vec<u8> {
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
        // Bradford D65 -> D50 chromatic adaptation matrix in s15Fixed16
        const CHAD: [i32; 9] = [
            0x00010c42, 0x000005de, -0x00000cdb, // 1.04788, 0.02292, -0.05022
            0x00000793, 0x0000fd90, -0x0000045f, // 0.02959, 0.99048, -0.01707
            -0x0000025e, 0x000003dc, 0x0000c06e, // -0.00925, 0.01508, 0.75168
        ];
        let mut b = Vec::with_capacity(44);
        b.extend_from_slice(b"sf32");
        push_be32(&mut b, 0);
        for v in CHAD {
            push_be32_i(&mut b, v);
        }
        b
    }
    fn make_srgb_para_tag() -> Vec<u8> {
        // ICC v4 parametricCurveType functionType 3 (IEC 61966-2.1 sRGB TRC):
        // Y = (a*X + b)^g for X >= d, else c*X
        let mut b = Vec::with_capacity(32);
        b.extend_from_slice(b"para");
        push_be32(&mut b, 0); // reserved
        push_be16(&mut b, 3); // functionType = 3
        push_be16(&mut b, 0); // reserved
        push_be32_i(&mut b, 0x00026666); // g = 2.4
        push_be32_i(&mut b, 0x0000f2a7); // a = 1 / 1.055 (0.94786)
        push_be32_i(&mut b, 0x00000d59); // b = 0.055 / 1.055 (0.05214), a + b = 1.0
        push_be32_i(&mut b, 0x000013d0); // c = 1 / 12.92 (0.07739)
        push_be32_i(&mut b, 0x00000a5b); // d = 0.04045
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
        // Unicode language(4) + count(4) + scriptCode(2) + macCount(1) + macName(67)
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
    let para = make_srgb_para_tag();
    let tag_defs: [(&[u8; 4], Vec<u8>); 7] = [
        (b"desc", make_desc_tag(desc)),
        (b"cprt", make_text_tag("Public Domain")),
        (b"wtpt", make_xyz_tag(D50_ICC)),
        (b"chad", make_chad_tag()),
        (b"rXYZ", make_xyz_tag(r_xyz)),
        (b"gXYZ", make_xyz_tag(g_xyz)),
        (b"bXYZ", make_xyz_tag(b_xyz)),
    ];

    let tag_count: u32 = 10; // 7 above + rTRC/gTRC/bTRC sharing `para`
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
    icc[8..12].copy_from_slice(&0x04000000u32.to_be_bytes()); // v4.0
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

fn display_p3_icc_profile() -> &'static [u8] {
    static ICC: OnceLock<Vec<u8>> = OnceLock::new();
    ICC.get_or_init(|| {
        // Bradford D65->D50 adapted Display P3 primaries (rXYZ, gXYZ, bXYZ columns)
        build_icc_v4(
            "Display P3",
            [33759, 15807, -69],
            [19135, 45367, 2745],
            [10296, 4363, 51385],
        )
    })
}

fn to_rgba16_with_fallback(img: &DecodedImage, ci_opt: Option<ColorInfo>) -> Option<heic_decoder::RgbaImage<u16>> {
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

#[inline]
fn srgb_to_linear(v: f32) -> f32 {
    let v = v.clamp(0.0, 1.0);
    if v <= 0.04045 {
        v / 12.92
    } else {
        ((v + 0.055) / 1.055).powf(2.4)
    }
}

/// Apply HDR tone-mapping (HLG / PQ) in native wide primaries, convert BT.2020 to Display P3,
/// encode with sRGB TRC and 4x4 Bayer ordered dithering, and output 8-bit/ch BGRA8 (32bppBGRA).
fn tone_map_hdr_to_bgra8(img: &DecodedImage, ci: ColorInfo) -> Option<(u32, u32, Vec<u8>)> {
    let rgba16 = to_rgba16_with_fallback(img, Some(ci))?;
    let w = rgba16.width as u32;
    let h = rgba16.height as u32;
    let premul = rgba16.premultiplied;
    let cp = ci.color_primaries;
    let tc = ci.transfer_characteristics;

    let (yr, yg, yb) = if cp == 9 {
        (0.2627_f32, 0.6780_f32, 0.0593_f32)
    } else {
        (0.2126_f32, 0.7152_f32, 0.0722_f32)
    };
    let convert_2020_to_p3 = cp == 9;

    let mut out = vec![0u8; (w as usize) * (h as usize) * 4];
    const INV_65535: f32 = 1.0 / 65535.0;
    let hlg_table = if tc == 18 { Some(hlg_lut()) } else { None };
    let srgb_table = srgb_sqrt_lut_255();
    let row_stride_src = (w as usize) * 4;
    let row_stride_dst = (w as usize) * 4;

    out.par_chunks_exact_mut(row_stride_dst)
        .zip(rgba16.data.par_chunks_exact(row_stride_src))
        .enumerate()
        .for_each(|(y, (dst_row, src_row))| {
            let bayer_row = &BAYER_4X4[y & 3];
            for x in 0..(w as usize) {
                let src = &src_row[x * 4..x * 4 + 4];
                let dst = &mut dst_row[x * 4..x * 4 + 4];
                let a16 = src[3];
                let no_premul = !(premul && a16 > 0 && a16 < 65535);

                let (mut lr, mut lg, mut lb) = match tc {
                    // HLG -> scene linear -> extended Reinhard shoulder in native gamut
                    18 if no_premul => {
                        let tbl = hlg_table.unwrap();
                        let sr = tbl[src[0] as usize];
                        let sg = tbl[src[1] as usize];
                        let sb = tbl[src[2] as usize];
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
                    }
                    18 => {
                        let af = a16 as f32 * INV_65535;
                        let r = (src[0] as f32 * INV_65535 / af).min(1.0);
                        let g = (src[1] as f32 * INV_65535 / af).min(1.0);
                        let b = (src[2] as f32 * INV_65535 / af).min(1.0);
                        let sr = hlg_inv_oetf(r);
                        let sg = hlg_inv_oetf(g);
                        let sb = hlg_inv_oetf(b);
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
                    }
                    // PQ -> SDR normalized -> extended Reinhard shoulder in native gamut
                    16 => {
                        let mut r = src[0] as f32 * INV_65535;
                        let mut g = src[1] as f32 * INV_65535;
                        let mut b = src[2] as f32 * INV_65535;
                        if !no_premul {
                            let af = a16 as f32 * INV_65535;
                            r = (r / af).min(1.0);
                            g = (g / af).min(1.0);
                            b = (b / af).min(1.0);
                        }
                        let sr = pq_eotf_sdr_norm(r);
                        let sg = pq_eotf_sdr_norm(g);
                        let sb = pq_eotf_sdr_norm(b);
                        let y_lum = yr * sr + yg * sg + yb * sb;
                        if y_lum > 1e-7 {
                            const WHITE: f32 = 5.0;
                            let l_out = y_lum * (1.0 + y_lum / (WHITE * WHITE)) / (1.0 + y_lum);
                            let s = l_out / y_lum;
                            (sr * s, sg * s, sb * s)
                        } else {
                            (0.0, 0.0, 0.0)
                        }
                    }
                    _ => unreachable!(),
                };

                if convert_2020_to_p3 {
                    (lr, lg, lb) = bt2020_to_display_p3_linear(lr, lg, lb);
                }

                let d = bayer_row[x & 3];
                dst[0] = dither_to_u8(linear_to_srgb_f255_fast(srgb_table, lb), d);
                dst[1] = dither_to_u8(linear_to_srgb_f255_fast(srgb_table, lg), d);
                dst[2] = dither_to_u8(linear_to_srgb_f255_fast(srgb_table, lr), d);
                dst[3] = ((a16 as u32 * 255 + 32767) / 65535) as u8;
            }
        });

    Some((w, h, out))
}

/// Convert high-bit-depth RGBA16 to dithered 8-bit/ch BGRA8 (32bppBGRA).
/// If `convert_2020_to_p3` is true (SDR BT.2020 without embedded ICC), maps primaries to Display P3.
fn rgba16_to_bgra8_dithered(
    rgba: &heic_decoder::RgbaImage<u16>,
    convert_2020_to_p3: bool,
) -> Vec<u8> {
    let w = rgba.width as usize;
    let h = rgba.height as usize;
    let mut out = vec![0u8; w * h * 4];
    let premul = rgba.premultiplied;
    const SCALE_255: f32 = 255.0 / 65535.0;
    const INV_65535: f32 = 1.0 / 65535.0;
    let srgb_table = if convert_2020_to_p3 {
        Some(srgb_sqrt_lut_255())
    } else {
        None
    };

    out.par_chunks_exact_mut(w * 4)
        .zip(rgba.data.par_chunks_exact(w * 4))
        .enumerate()
        .for_each(|(y, (dst_row, src_row))| {
            let bayer_row = &BAYER_4X4[y & 3];
            for x in 0..w {
                let src = &src_row[x * 4..x * 4 + 4];
                let dst = &mut dst_row[x * 4..x * 4 + 4];
                let a16 = src[3];
                let (mut r_u16, mut g_u16, mut b_u16) = (src[0], src[1], src[2]);
                if premul && a16 > 0 && a16 < 65535 {
                    let a32 = a16 as u32;
                    r_u16 = (((r_u16 as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                    g_u16 = (((g_u16 as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                    b_u16 = (((b_u16 as u32) * 65535 + a32 / 2) / a32).min(65535) as u16;
                }

                let d = bayer_row[x & 3];
                if let Some(tbl) = srgb_table {
                    let lr = srgb_to_linear(r_u16 as f32 * INV_65535);
                    let lg = srgb_to_linear(g_u16 as f32 * INV_65535);
                    let lb = srgb_to_linear(b_u16 as f32 * INV_65535);
                    let (pr, pg, pb) = bt2020_to_display_p3_linear(lr, lg, lb);
                    dst[0] = dither_to_u8(linear_to_srgb_f255_fast(tbl, pb), d);
                    dst[1] = dither_to_u8(linear_to_srgb_f255_fast(tbl, pg), d);
                    dst[2] = dither_to_u8(linear_to_srgb_f255_fast(tbl, pr), d);
                } else {
                    dst[0] = dither_to_u8(b_u16 as f32 * SCALE_255, d);
                    dst[1] = dither_to_u8(g_u16 as f32 * SCALE_255, d);
                    dst[2] = dither_to_u8(r_u16 as f32 * SCALE_255, d);
                }
                dst[3] = ((a16 as u32 * 255 + 32767) / 65535) as u8;
            }
        });
    out
}

#[no_mangle]
pub unsafe extern "C" fn heif_wic_decode(
    input_ptr: *const u8,
    input_len: usize,
    out: *mut HeifWicDecoded,
) -> i32 {
    if input_ptr.is_null() || input_len == 0 || out.is_null() {
        return -1;
    }

    let bytes = slice::from_raw_parts(input_ptr, input_len);
    let Ok(img) = heic_decoder::decode(bytes) else {
        return -2;
    };

    let is_high_bpc = img.bit_depth_luma > 8 || img.bit_depth_chroma > 8;
    let embedded_icc = img.metadata.icc_profile.clone();
    let ci_opt = img.color_info;

    let is_hdr_tc = embedded_icc.is_none()
        && ci_opt.map_or(false, |ci| {
            ci.transfer_characteristics == 18 || ci.transfer_characteristics == 16
        });

    let cp_no_icc = if embedded_icc.is_none() {
        ci_opt.map(|c| c.color_primaries)
    } else {
        None
    };

    let (w, h, bpc_out, mut bgra, synthesized_icc): (u32, u32, u32, Vec<u8>, Option<&'static [u8]>) =
        if is_hdr_tc {
            let ci = ci_opt.unwrap();
            let Some((w, h, buf)) = tone_map_hdr_to_bgra8(&img, ci) else {
                return -3;
            };
            let icc = match ci.color_primaries {
                9 | 12 => Some(display_p3_icc_profile()),
                _ => None,
            };
            (w, h, 8, buf, icc)
        } else if is_high_bpc || cp_no_icc == Some(9) {
            let Some(rgba16) = to_rgba16_with_fallback(&img, ci_opt) else {
                return -3;
            };
            let convert_2020_to_p3 = cp_no_icc == Some(9);
            let buf = rgba16_to_bgra8_dithered(&rgba16, convert_2020_to_p3);
            let icc = match cp_no_icc {
                Some(9) | Some(12) => Some(display_p3_icc_profile()),
                _ => None,
            };
            (rgba16.width as u32, rgba16.height as u32, 8, buf, icc)
        } else {
            let rgba_res = img.to_rgba8().or_else(|_| {
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
                img.to_rgba8_with_options(&RgbOptions {
                    color: Some(fallback),
                    upsampling: ChromaUpsampling::Bilinear,
                })
            });

            let Ok(mut rgba) = rgba_res else {
                return -3;
            };

            let w = rgba.width as u32;
            let h = rgba.height as u32;
            let premul = rgba.premultiplied;

            if premul {
                for px in rgba.data.chunks_exact_mut(4) {
                    let r = px[0];
                    let g = px[1];
                    let b = px[2];
                    let a = px[3];
                    if a > 0 && a < 255 {
                        let a16 = a as u16;
                        px[0] = (((b as u16) * 255 + a16 / 2) / a16).min(255) as u8;
                        px[1] = (((g as u16) * 255 + a16 / 2) / a16).min(255) as u8;
                        px[2] = (((r as u16) * 255 + a16 / 2) / a16).min(255) as u8;
                    } else {
                        px[0] = b;
                        px[2] = r;
                    }
                }
            } else {
                for px in rgba.data.chunks_exact_mut(4) {
                    px.swap(0, 2);
                }
            }

            let icc = match cp_no_icc {
                Some(12) => Some(display_p3_icc_profile()),
                _ => None,
            };
            (w, h, 8, rgba.data, icc)
        };

    let bgra_len = bgra.len();
    let bgra_ptr = bgra.as_mut_ptr();
    std::mem::forget(bgra);

    let icc_vec = embedded_icc.or_else(|| synthesized_icc.map(|s| s.to_vec()));
    let (icc_ptr, icc_len) = if let Some(mut icc) = icc_vec {
        let len = icc.len();
        let ptr = icc.as_mut_ptr();
        std::mem::forget(icc);
        (ptr, len)
    } else {
        (ptr::null_mut(), 0)
    };

    let bpp = if bpc_out == 16 { 8 } else { 4 };
    *out = HeifWicDecoded {
        width: w,
        height: h,
        stride: w * bpp,
        bits_per_channel: bpc_out,
        bgra_ptr,
        bgra_len,
        icc_ptr,
        icc_len,
    };

    0
}

#[no_mangle]
pub unsafe extern "C" fn heif_wic_free(decoded: *mut HeifWicDecoded) {
    if decoded.is_null() {
        return;
    }
    let d = &mut *decoded;
    if !d.bgra_ptr.is_null() && d.bgra_len > 0 {
        let _ = Vec::from_raw_parts(d.bgra_ptr, d.bgra_len, d.bgra_len);
        d.bgra_ptr = ptr::null_mut();
        d.bgra_len = 0;
    }
    if !d.icc_ptr.is_null() && d.icc_len > 0 {
        let _ = Vec::from_raw_parts(d.icc_ptr, d.icc_len, d.icc_len);
        d.icc_ptr = ptr::null_mut();
        d.icc_len = 0;
    }
}
