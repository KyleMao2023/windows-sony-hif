# heif-ps-plugin — Native Adobe Photoshop Sony `.HIF` Plugin (`SonyHifFormat.8bi`)

[简体中文](README.zh-CN.md)

A native 64-bit Adobe Photoshop **File Format Plugin** (`SonyHifFormat.8bi`) designed exclusively for **Sony `.HIF` (`heix + SHIF`)** photos, giving Photoshop direct access to 10-bit 4:2:0 / 4:2:2 / 4:2:2 HLG data without going through Windows 8-bit WIC truncation.

## Tested Environment

- **Editor**: Adobe Photoshop 2026 (`x64`) on Windows 11 x64
- **SDK**: Built against [AdobeDocs/photoshop-cpp-sdk](https://github.com/AdobeDocs/photoshop-cpp-sdk) (`JSON_PIPL` format descriptor)

## Features & Bit-Depth Modes

- **Exclusive `.HIF` Routing**: PiPL declares `ReadExtensions: ["HIF "]` so standard `.heic` / `.heif` workflows are completely unaffected, while opening or dragging any `.HIF` directly into Photoshop launches the plugin.
- **Interactive Options Dialog (`IDD_HIF_OPTIONS`)**: Lets you pick the color pipeline per file:
  1. **32-bit Float Linear HDR (`RGB/32*`)** *(recommended for HLG post-processing)*: Applies the **ITU-R BT.2408 HLG OOTF** (`1000/203 * Y^0.2`, diffuse white at `1.0`), tags **ITU-R BT.2020 Linear ICC**, preserving full dynamic range for HDR toning, Exposure, and Camera Raw filters.
  2. **16-bit Integer Tone-Mapped (`RGB/16*`)**: Applies smooth Reinhard HDR→SDR roll-off with **BT.2020 (sRGB TRC)** ICC profile for direct editing without HDR adjustments.
  3. **16-bit Integer Raw Signal (`RGB/16*`)**: Applies pure HLG inverse OETF without system gamma boost in BT.2020 for custom grading or LUTs.
- **Full EXIF & XMP Passthrough**: Reads Sony EXIF and XMP packets directly from the `.HIF` container, passes them via `propEXIFData` and `propXMP`, applies physical pixel orientation, and normalizes EXIF Orientation tag to `1` to prevent double rotation.

## Installation & Uninstallation

### Install (Administrator PowerShell)

Close Photoshop first, then run:

```powershell
.\install.ps1
```

`install.ps1` automatically locates your Photoshop `Plug-ins` directory (or pass `-PluginsDir "D:\Program Files\Adobe\Adobe Photoshop 2026\Plug-ins"` explicitly) and copies `SonyHifFormat.8bi`.

### Uninstall

Close Photoshop and run:

```powershell
.\uninstall.ps1
```

## Modifying & Building from Source

### Prerequisites

1. **Rust** (`x86_64-pc-windows-gnu`) + **MinGW-w64** (`g++`, `windres`)
2. Official **Adobe Photoshop C++ SDK** (not bundled in this repository due to Adobe's SDK License Agreement):
   ```powershell
   git clone https://github.com/AdobeDocs/photoshop-cpp-sdk ..\sdk\photoshop-cpp-sdk
   ```

### Build & Test

```powershell
.\build.ps1 -PhotoshopSdkDir "<path-to-photoshop-cpp-sdk>"
```

Run the standalone host test harness (verifies all 3 bit-depth modes, preview/revert path, Windows WCS ICC validation, and EXIF passthrough without launching Photoshop):

```powershell
.\build\test_ps_plugin.exe "path\to\sample.HIF"
```

### Customization Entry Points

- **HLG OOTF / Tone Curves / ICC Profiles**: `rust-sony-hif/src/lib.rs` (`sony_hif_decode` modes `0`, `1`, `2`).
- **Photoshop UI Dialog & Host Callbacks**: `src/SonyHifFormat.cpp` & `src/SonyHifFormat.rc`.
- **Supported Extensions & File Format Capabilities**: `src/PiPLs.json`.
