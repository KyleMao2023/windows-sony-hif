# Windows Sony HEIF Suite（Windows 索尼 `.HIF` 看图与 Photoshop 全链路套件）

[English](README.md)

为 Windows 桌面端与专业修图打通 **Sony `.HIF`（`heix + SHIF`）** 相机照片——完整支持 **10-bit 4:2:0、4:2:2 与 4:2:2 HLG**——覆盖 **Windows 资源管理器 / 照片应用** 以及 **Adobe Photoshop**。

## 测试环境

- **系统**：Windows 11 x64（建议在“系统 → 屏幕 → 颜色管理”中关闭“自动管理应用的颜色 (ACM)”，避免 Windows 照片异步叠加 HLG 压暗逻辑）
- **后期软件**：Adobe Photoshop 2026（`x64`）
- **编译环境**：Rust（`x86_64-pc-windows-gnu`）+ MinGW-w64 `g++` / `windres` + 官方 [AdobeDocs/photoshop-cpp-sdk](https://github.com/AdobeDocs/photoshop-cpp-sdk)

## 包含组件

| 组件 | 类型 | 功能说明 | 说明文档 |
| --- | --- | --- | --- |
| **heif-wic** | Windows WIC COM 解码器（`heif_wic.dll`） | 注册原生 WIC 解码组件接管 Sony `.HIF` 与 HEIF：内部全 16-bit/`f32` 解码后以 **Display P3 ICC v4** 输出 `32bppBGRA`（带 4×4 Bayer 有序抖动防断层 + Reinhard HDR→SDR 色调映射），并完整透传 EXIF / 旋转方向；严格放行 AVIF。 | [English](heif-wic/README.md) · [中文](heif-wic/README.zh-CN.md) |
| **heif-ps-plugin** | Photoshop 原生格式插件（`SonyHifFormat.8bi`） | 独占接管 `.HIF` 后缀（“打开”或拖入 Photoshop 即触发）。弹窗自选 **32-bit 浮点线性 HDR（BT.2020 + BT.2408 HLG OOTF）**、**16-bit 色调映射版** 或 **16-bit 原始信号版**，并完整带入 EXIF 与 XMP 元数据。 | [English](heif-ps-plugin/README.md) · [中文](heif-ps-plugin/README.zh-CN.md) |

> 配套手机端模块（Creators' App Wi-Fi 导入 `.HIF` + Zygisk 安卓系统解锁）：[android-sony-hif](https://github.com/KyleMao2023/android-sony-hif)。

## 快速安装（使用 Release 预编译包）

1. 下载 Release 压缩包：
   - `sony-heif-windows-wic-0.1.0-win64.zip`
   - `sony-heif-photoshop-plugin-0.1.0-win64.zip`
2. 分别解压后，以**管理员权限打开 PowerShell** 运行目录内的 `install.ps1`。
3. 如需卸载或验证效果，请参考各子目录 README。

## 从源码编译与二次开发

### 环境依赖

- **Rust**：`stable-x86_64-pc-windows-gnu`（或安装目标 `rustup target add x86_64-pc-windows-gnu`）
- **MinGW-w64**：`g++`（C++17）与 `windres` 在系统 `PATH` 中
- **Adobe Photoshop C++ SDK**（仅编译 `heif-ps-plugin` 需要）：自行克隆 [AdobeDocs/photoshop-cpp-sdk](https://github.com/AdobeDocs/photoshop-cpp-sdk)

```powershell
git clone https://github.com/AdobeDocs/photoshop-cpp-sdk sdk\photoshop-cpp-sdk
.\build-all.ps1
```

编译产物位于各模块 `build/` 目录，打包后的 Release 压缩包生成在 `dist/`。

## 开源许可

MIT License —— 详见 [LICENSE](LICENSE) 与 [THIRD_PARTY.md](THIRD_PARTY.md)。
