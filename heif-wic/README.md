# heif-wic — Windows WIC Sony `.HIF` / HEIF Decoder

[简体中文](README.zh-CN.md)

A lightweight Windows Imaging Component (WIC) COM decoder (`heif_wic.dll`) powered by pure-Rust [heic-decoder](https://github.com/Multipad-cyber/heic-decoder-rust). Enables **Windows Explorer thumbnails** and **Microsoft Photos** to open 10-bit 4:2:0 / 4:2:2 / 4:2:2 HLG Sony `.HIF` files cleanly without requiring Windows Store extensions.

## Tested Environment

- **OS**: Windows 11 x64 + Microsoft Photos + Windows Explorer
- **Display Note**: For Sony HLG `.HIF` photos, turn **off** Windows Settings → System → Display → Color management → **Auto Color Management (ACM)** if Microsoft Photos applies an asynchronous darkening pass ~0.5s after opening `.HIF` due to `colr/nclx (tc=18)`.

## Design & Color Pipeline

- **Format Matching & Priority**: Registers `.hif`, `.heic`, `.heif` container patterns (`ftyp` + `heic`/`heix`/`mif1`/`msf1`...) with `ArbitrationPriority = 10` (`HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\...` + WIC `BitmapDecoders` registration), while strictly rejecting `avif`/`avis` brands so stock AVIF decoders remain unaffected.
- **Pixel Output**: Decodes internally in 16-bit / `f32` precision, applies **BT.2408 HLG scene recovery → Reinhard HDR→SDR tone mapping (`GAIN=4.8`) → BT.2020-to-Display P3 gamut transform**, and converts to `32bppBGRA` with a zero-mean **4×4 Bayer ordered dither** to eliminate 8-bit sky banding.
- **Color Profile**: Embeds a spec-compliant ICC v4 **Display P3** profile (`desc` + `text` + `para` sRGB TRC), passing Windows WIC/WCS/Direct2D color-management verification.
- **Metadata**: Parses EXIF TIFF/Orientation from `Exif` items and exposes them via `IWICMetadataQueryReader` so portrait photos rotate automatically in Explorer and Photos.

## Right-Click Context Menu sRGB JPEG Exporter (`heif_export.exe`)

After installation, right-clicking any `.hif` / `.heif` / `.heic` file in Windows Explorer (under **"Show more options"** or **`Shift + Right-Click`** on Windows 11) shows a cascading **"导出为 sRGB JPG"** submenu with **2 × 3 = 6** presets:

- **Reinhard - 原图 (100%)** → `<name>_reinhard.jpg` (neutral look with open shadows)
- **Reinhard - 50%** → `<name>_reinhard_50.jpg`
- **Reinhard - 25%** → `<name>_reinhard_25.jpg` (compact, ready for instant messaging)
- **ACES Filmic - 原图 (100%)** → `<name>_aces.jpg` (Stephen Hill ACES Fitted S-curve contrast)
- **ACES Filmic - 50%** → `<name>_aces_50.jpg`
- **ACES Filmic - 25%** → `<name>_aces_25.jpg`

Highlights:
- Reuses the Rust static decoder + Windows WIC Fant downscaler & JPEG encoder (`quality=92`) with no console window flash (~1.1–1.3s per 60MP Sony `.HIF`);
- Converts pixels to standard **sRGB** with an embedded `sRGB IEC61966-2.1` ICC v4 profile so chat apps and mobile viewers display accurate colors even if ICC is ignored;
- Serializes multi-file selections via a named mutex to avoid CPU/RAM spikes;
- Also supports CLI invocation:
  ```powershell
  "C:\Program Files\CustomHeifWic\heif_export.exe" --tonemap aces --scale 50 --quality 92 "DSC06556.HIF"
  ```

## Installation & Uninstallation

### Install (Administrator PowerShell)

```powershell
.\install.ps1
```

What `install.ps1` does:
1. Stops running Photos processes to unlock the target file.
2. Copies `heif_wic.dll` and `heif_export.exe` to `C:\Program Files\CustomHeifWic\`.
3. Grants `Read & Execute` (`*S-1-15-2-1:(OI)(CI)(RX)`) ACL so UWP apps like Microsoft Photos can load the COM DLL.
4. Registers the COM server via `regsvr32.exe /s` and registers the 6-item Explorer context menu via `heif_export.exe --register-menu` (pass `-SkipContextMenu` to skip menu registration).

Optionally verify which decoder Windows selects for your photos:

```powershell
.\install.ps1 -VerifyFiles "path\to\photo.HIF","path\to\photo.HEIC"
```

### Uninstall

```powershell
.\uninstall.ps1
```

Removes the Explorer export context menu, unregisters the COM server (`regsvr32 /s /u`), and removes `C:\Program Files\CustomHeifWic`. (If Explorer still holds a handle on `heif_wic.dll`, restart Explorer or reboot once, then delete the folder.)

## Modifying & Building from Source

```powershell
.\build.ps1
```

Outputs `build\heif_wic.dll`, `build\heif_export.exe`, `build\test_heif_local.exe`, and `build\verify_heif_wic.exe`.

### Customization Entry Points

- **Tone Mapping / Target Gamut**: `rust-heif-dec/src/lib.rs` (`heif_wic_decode_ex` supports Reinhard / ACES Filmic and Display P3 / sRGB).
- **Context Menu & Export Quality**: `heif_export.cpp` (default JPEG quality `0.92f` and `RegisterContextMenu`).
- **Registered Extensions & File Patterns**: `heif_wic.cpp` (`DllRegisterServer` registry keys and `MatchPattern` / `QueryCapability`).
