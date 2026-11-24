# Windows Sony HEIF Suite

[简体中文](README.zh-CN.md)

Native Windows decoding and editing support for **Sony `.HIF` (`heix + SHIF`)** camera photos—covering **10-bit 4:2:0, 4:2:2, and 4:2:2 HLG**—for both **Windows Explorer / Microsoft Photos** and **Adobe Photoshop**.

## Tested Environment

- **OS**: Windows 11 x64 (`24H2` / WIC & Windows Photos; disable Windows Auto Color Management (ACM) in Display settings for consistent SDR viewing without Photos async HLG darken pass)
- **Editor**: Adobe Photoshop 2026 (`x64`)
- **Compiler Toolchain**: Rust (`x86_64-pc-windows-gnu`) + MinGW-w64 `g++` / `windres` + official [AdobeDocs/photoshop-cpp-sdk](https://github.com/AdobeDocs/photoshop-cpp-sdk)

## Components

| Component | Type | What It Does | Docs |
| --- | --- | --- | --- |
| **heif-wic** | Windows WIC COM Codec (`heif_wic.dll`) | Registers a native Windows Imaging Component decoder (`CLSID_CustomHeifDecoder`) with elevated merit for Sony `.HIF` & HEIF images, rendering 60MP 10-bit 4:2:2 / HLG photos to **Display P3** (`32bppBGRA` with 4×4 Bayer ordered dithering and Reinhard HDR→SDR tone mapping) plus full EXIF/Orientation passthrough. Leaves AVIF untouched. | [English](heif-wic/README.md) · [中文](heif-wic/README.zh-CN.md) |
| **heif-ps-plugin** | Photoshop Format Plugin (`SonyHifFormat.8bi`) | Exclusive handler for `.HIF` inside Adobe Photoshop (`Open` / drag-and-drop). Displays an interactive bit-depth dialog allowing **32-bit Float Linear HDR (BT.2020, BT.2408 HLG OOTF)**, **16-bit Integer Tone-Mapped**, or **16-bit Integer Raw Signal**, with full EXIF & XMP metadata passthrough. | [English](heif-ps-plugin/README.md) · [中文](heif-ps-plugin/README.zh-CN.md) |

> Android companion modules (Sony Creators' App Wi-Fi HEIF transfer + Zygisk Android system decoder) live in [android-sony-hif](https://github.com/KyleMao2023/android-sony-hif).

## Quick Start (Prebuilt Releases)

1. Download the release ZIPs:
   - `sony-heif-windows-wic-0.1.0-win64.zip`
   - `sony-heif-photoshop-plugin-0.1.0-win64.zip`
2. Extract each ZIP and run its `install.ps1` in an **elevated PowerShell** (Run as Administrator).
3. See each subproject's README for verification and clean uninstallation instructions.

## Building Everything from Source

### Prerequisites

- **Rust**: `stable-x86_64-pc-windows-gnu` (or `stable` with `rustup target add x86_64-pc-windows-gnu`)
- **MinGW-w64**: `g++` (C++17) and `windres` on `PATH`
- **Adobe Photoshop C++ SDK** (required only for `heif-ps-plugin`): clone [AdobeDocs/photoshop-cpp-sdk](https://github.com/AdobeDocs/photoshop-cpp-sdk) locally

```powershell
git clone https://github.com/AdobeDocs/photoshop-cpp-sdk sdk\photoshop-cpp-sdk
.\build-all.ps1
```

Outputs are compiled under `*/build/` and packaged into `dist/`.

## License

MIT License — see [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).
