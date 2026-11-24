# heif-ps-plugin —— Adobe Photoshop 原生索尼 `.HIF` 插件（`SonyHifFormat.8bi`）

[English](README.md)

专为 **Sony `.HIF`（`heix + SHIF`）** 照片打造的 64 位原生 Adobe Photoshop **文件格式插件（Format Plug-in）**。绕过 Windows WIC 的 8-bit 截断，将 10-bit 4:2:0 / 4:2:2 / 4:2:2 HLG 完整动态范围与色彩信息直接送入 Photoshop。

## 测试环境

- **软件**：Windows 11 x64 + Adobe Photoshop 2026（`x64`）
- **开发套件**：基于官方 [AdobeDocs/photoshop-cpp-sdk](https://github.com/AdobeDocs/photoshop-cpp-sdk)（使用 `JSON_PIPL` 描述符）

## 核心特性与位深模式

- **只接管 `.HIF`**：`PiPLs.json` 仅注册 `ReadExtensions: ["HIF "]`，不干预普通的 `.heic` / `.heif` 工作流；将 `.HIF` 直接“打开”或拖入 Photoshop 即可触发本插件。
- **交互式导入选项弹窗（`IDD_HIF_OPTIONS`）**：每次打开可自由选择三种色彩管线：
  1. **32-bit 浮点线性 HDR（`RGB/32*`）** *(推荐用于 HLG 后期)*：严格执行 **ITU-R BT.2408 HLG OOTF**（漫反射白 `= 1.0`，峰值 `1000/203 ≈ 4.926`），附带 **BT.2020 线性 ICC**，完整保留高光动态范围以供 HDR 调色与曝光调整。
  2. **16-bit 整数色调映射版（`RGB/16*`）**：经 Reinhard HDR→SDR 平滑压暗高光后输出，附带 **BT.2020 (sRGB TRC)** ICC，打开即呈现自然影调。
  3. **16-bit 整数原始信号版（`RGB/16*`）**：仅做纯 HLG 逆 OETF 还原线性场景光（不叠加 OOTF 系统伽马），方便套用自定义 LUT。
- **完整 EXIF / XMP 与方向处理**：直接从 `.HIF` 容器提取原始 EXIF 与 XMP 写入 Photoshop 文档属性（`propEXIFData`、`propXMP`），在解码阶段完成像素级物理旋转并将 EXIF Orientation 标签归位为 `1`，避免二次旋转。

## 安装与卸载

### 安装（管理员权限 PowerShell）

请先关闭 Photoshop，然后执行：

```powershell
.\install.ps1
```

`install.ps1` 会自动探测本机 Photoshop 的 `Plug-ins` 目录并复制 `SonyHifFormat.8bi`；也可通过 `-PluginsDir "<Photoshop 安装路径>\Plug-ins"` 手动指定。

### 卸载

关闭 Photoshop 后执行：

```powershell
.\uninstall.ps1
```

## 修改与从源码编译

### 环境准备

1. **Rust**（`x86_64-pc-windows-gnu`）+ **MinGW-w64**（`g++`、`windres`）
2. 自行下载官方 **Adobe Photoshop C++ SDK**（受 Adobe SDK 协议限制，本仓库不直接打包 SDK 头文件）：
   ```powershell
   git clone https://github.com/AdobeDocs/photoshop-cpp-sdk ..\sdk\photoshop-cpp-sdk
   ```

### 编译与独立测试

```powershell
.\build.ps1 -PhotoshopSdkDir "<photoshop-cpp-sdk 路径>"
```

可使用内置的主机测试宿主（无需启动 Photoshop，即可验证三种位深模式、WCS ICC 合法性与 EXIF 透传）：

```powershell
.\build\test_ps_plugin.exe "你的样片.HIF"
```

### 常见二次开发入口

- **HLG OOTF / 色调曲线 / ICC**：`rust-sony-hif/src/lib.rs`（`sony_hif_decode` 的模式 `0`、`1`、`2`）。
- **Photoshop 弹窗 UI 与宿主回调**：`src/SonyHifFormat.cpp`、`src/SonyHifFormat.rc`。
- **支持后缀与格式能力声明**：`src/PiPLs.json`。
