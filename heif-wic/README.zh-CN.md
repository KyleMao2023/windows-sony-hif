# heif-wic —— Windows WIC 索尼 `.HIF` / HEIF 解码器

[English](README.md)

基于纯 Rust 解码库 [heic-decoder](https://github.com/Multipad-cyber/heic-decoder-rust) 封装的 Windows Imaging Component（WIC）COM 解码器（`heif_wic.dll`）。安装后，**Windows 资源管理器缩略图**与**微软照片（Photos）**即可直接流畅浏览 Sony 10-bit 4:2:0 / 4:2:2 / 4:2:2 HLG `.HIF` 照片。

## 测试环境

- **系统与看图**：Windows 11 x64、Windows 资源管理器、Microsoft Photos
- **显色建议**：查看 Sony HLG `.HIF` 时，建议关闭 Windows“设置 → 系统 → 显示 → 颜色管理 → **自动管理应用的颜色 (ACM)**”，否则微软照片会在开图约 0.5 秒后读取容器 `colr/nclx (tc=18)` 叠加额外的异步压暗。

## 技术设计与色彩管线

- **格式匹配与优先级**：向 WIC `BitmapDecoders` 注册 `.hif`、`.heic`、`.heif` 后缀及 ISO BMFF `ftyp` 模式匹配（`ArbitrationPriority = 10`），同时严格排除 `avif`/`avis` 品牌，不抢占系统或第三方 AVIF 解码。
- **高色准像素管线**：内部以 16-bit / `f32` 高精度解码；对 HLG 照片执行 **BT.2408 线性逆变换 → Reinhard HDR→SDR 色调映射（`GAIN=4.8`）→ BT.2020 至 Display P3 色域转换**，并通过零均值 **4×4 Bayer 有序抖动**量化至 `32bppBGRA`，消除 8-bit 蓝天断层。
- **标准 ICC v4 嵌入**：内嵌通过 Windows WCS/D2D 校验的 **Display P3 ICC v4** 描述文件（完整 `desc`、`text`、`para` sRGB TRC 标签）。
- **元数据与方向**：解析 `Exif` 块中的方向（Orientation）并通过 `IWICMetadataQueryReader` 暴露给系统，保证竖拍照片在资源管理器与照片应用中自动摆正。

## 安装与卸载

### 安装（管理员权限 PowerShell）

```powershell
.\install.ps1
```

`install.ps1` 会执行：
1. 结束可能占用旧 DLL 的 `Photos` 进程；
2. 将 `heif_wic.dll` 复制到 `C:\Program Files\CustomHeifWic\heif_wic.dll`；
3. 为 `ALL APPLICATION PACKAGES`（`*S-1-15-2-1`）授予读取与执行权限，使 UWP 版“微软照片”能够正常加载该 COM 组件；
4. 调用 `regsvr32.exe /s` 注册 WIC 解码器。

如需验证系统是否已优先选中本解码器：

```powershell
.\install.ps1 -VerifyFiles "你的照片.HIF","普通手机.HEIC"
```

### 卸载

```powershell
.\uninstall.ps1
```

会反注册 COM 组件并删除 `C:\Program Files\CustomHeifWic`。（若资源管理器仍占用 DLL 句柄，重启资源管理器或重启电脑后再删目录即可。）

## 修改与从源码编译

```powershell
.\build.ps1
```

产物生成在 `build\heif_wic.dll`、本地加载验证工具 `build\test_heif_local.exe` 以及系统仲裁检查工具 `build\verify_heif_wic.exe`。

### 常见二次开发入口

- **Tone Mapping 曲线与输出色域**：`rust-heif-dec/src/lib.rs`（可修改 `GAIN`、替换为 BT.2390/ACES/AgX，或切换目标 ICC 与色域矩阵）。
- **接管的后缀与优先级**：`heif_wic.cpp`（`DllRegisterServer` 注册表项与 `QueryCapability`）。
