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

## Installation & Uninstallation

### Install (Administrator PowerShell)

```powershell
.\install.ps1
```

What `install.ps1` does:
1. Stops running Photos processes to unlock the target file.
2. Copies `heif_wic.dll` to `C:\Program Files\CustomHeifWic\heif_wic.dll`.
3. Grants `Read & Execute` (`*S-1-15-2-1:(OI)(CI)(RX)`) ACL so UWP apps like Microsoft Photos can load the COM DLL.
4. Registers the COM server & WIC decoder metadata via `regsvr32.exe /s`.

Optionally verify which decoder Windows selects for your photos:

```powershell
.\install.ps1 -VerifyFiles "path\to\photo.HIF","path\to\photo.HEIC"
```

### Uninstall

```powershell
.\uninstall.ps1
```

Unregisters the COM server (`regsvr32 /s /u`) and removes `C:\Program Files\CustomHeifWic`. (If Explorer still holds a handle on `heif_wic.dll`, restart Explorer or reboot once, then delete the folder.)

## Modifying & Building from Source

```powershell
.\build.ps1
```

Outputs `build\heif_wic.dll`, `build\test_heif_local.exe` (direct `DllGetClassObject` loader test), and `build\verify_heif_wic.exe` (system-wide `CreateDecoderFromFilename` arbiter check).

### Customization Entry Points

- **Tone Mapping / Target Gamut**: `rust-heif-dec/src/lib.rs` (adjust `GAIN`, swap Reinhard for BT.2390/ACES/AgX, or change output gamut matrix & ICC profile bytes).
- **Registered Extensions & File Patterns**: `heif_wic.cpp` (`DllRegisterServer` registry keys and `MatchPattern` / `QueryCapability`).
