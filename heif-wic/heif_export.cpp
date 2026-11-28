#define INITGUID
#include <windows.h>
#include <wincodec.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

extern "C" {
struct HeifWicDecoded {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t bits_per_channel;
    uint8_t* bgra_ptr;
    size_t bgra_len;
    uint8_t* icc_ptr;
    size_t icc_len;
};

static constexpr uint32_t HEIF_TONEMAP_REINHARD = 0;
static constexpr uint32_t HEIF_TONEMAP_ACES_FILMIC = 1;
static constexpr uint32_t HEIF_GAMUT_DISPLAY_P3 = 0;
static constexpr uint32_t HEIF_GAMUT_SRGB = 1;

int32_t heif_wic_decode_ex(
    const uint8_t* input_ptr,
    size_t input_len,
    uint32_t tone_map,
    uint32_t target_gamut,
    HeifWicDecoded* out);
void heif_wic_free(HeifWicDecoded* decoded);
}

struct ExportOptions {
    uint32_t toneMap = HEIF_TONEMAP_REINHARD;
    uint32_t scalePercent = 100;
    float jpegQuality = 0.92f;
    bool quiet = false;
    bool registerMenu = false;
    bool unregisterMenu = false;
    std::wstring explicitOutput;
    std::vector<std::wstring> inputs;
};

struct DecodedGuard {
    HeifWicDecoded data = {};
    ~DecodedGuard() {
        heif_wic_free(&data);
    }
};

struct MutexGuard {
    HANDLE h = NULL;
    bool locked = false;
    explicit MutexGuard(const WCHAR* name) {
        h = CreateMutexW(NULL, FALSE, name);
        if (h) {
            DWORD r = WaitForSingleObject(h, INFINITE);
            if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
                locked = true;
            }
        }
    }
    ~MutexGuard() {
        if (h) {
            if (locked) ReleaseMutex(h);
            CloseHandle(h);
        }
    }
};

static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out, std::wstring& err) {
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        err = L"无法打开源文件: " + path;
        return false;
    }

    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (512LL * 1024 * 1024)) {
        CloseHandle(hFile);
        err = L"源文件大小无效: " + path;
        return false;
    }

    out.resize((size_t)sz.QuadPart);
    size_t offset = 0;
    while (offset < out.size()) {
        DWORD toRead = (DWORD)((out.size() - offset) > (16u * 1024u * 1024u)
            ? (16u * 1024u * 1024u)
            : (out.size() - offset));
        DWORD got = 0;
        if (!ReadFile(hFile, out.data() + offset, toRead, &got, NULL) || got == 0) {
            CloseHandle(hFile);
            err = L"读取源文件失败: " + path;
            return false;
        }
        offset += got;
    }
    CloseHandle(hFile);
    return true;
}

static std::wstring BuildDefaultOutputPath(const std::wstring& inputPath, const ExportOptions& opt) {
    size_t slash = inputPath.find_last_of(L"\\/");
    size_t dot = inputPath.find_last_of(L'.');
    std::wstring base = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        ? inputPath.substr(0, dot)
        : inputPath;

    const WCHAR* tmTag = (opt.toneMap == HEIF_TONEMAP_ACES_FILMIC) ? L"_aces" : L"_reinhard";
    std::wstring scaleTag;
    if (opt.scalePercent != 100) {
        scaleTag = L"_" + std::to_wstring(opt.scalePercent);
    }
    return base + tmTag + scaleTag + L".jpg";
}

static bool ExportOneFile(
    IWICImagingFactory* factory,
    const std::wstring& inputPath,
    const std::wstring& outputPath,
    const ExportOptions& opt,
    std::wstring& err)
{
    std::vector<uint8_t> raw;
    if (!ReadWholeFile(inputPath, raw, err)) {
        return false;
    }

    DecodedGuard decoded;
    int32_t rc = heif_wic_decode_ex(
        raw.data(), raw.size(), opt.toneMap, HEIF_GAMUT_SRGB, &decoded.data);
    raw.clear();
    raw.shrink_to_fit();

    if (rc != 0 || !decoded.data.bgra_ptr || decoded.data.width == 0 || decoded.data.height == 0) {
        WCHAR buf[128];
        swprintf(buf, 128, L"解码失败 (code=%d): ", rc);
        err = std::wstring(buf) + inputPath;
        return false;
    }

    IWICBitmap* srcBitmap = NULL;
    HRESULT hr = factory->CreateBitmapFromMemory(
        decoded.data.width,
        decoded.data.height,
        GUID_WICPixelFormat32bppBGRA,
        decoded.data.stride,
        (UINT)decoded.data.bgra_len,
        decoded.data.bgra_ptr,
        &srcBitmap);
    if (FAILED(hr)) {
        err = L"创建 WIC 位图失败。";
        return false;
    }

    IWICBitmapSource* encodeSource = srcBitmap;
    IWICBitmapScaler* scaler = NULL;

    if (opt.scalePercent != 100) {
        UINT dstW = (UINT)(((uint64_t)decoded.data.width * opt.scalePercent + 50) / 100);
        UINT dstH = (UINT)(((uint64_t)decoded.data.height * opt.scalePercent + 50) / 100);
        if (dstW == 0) dstW = 1;
        if (dstH == 0) dstH = 1;

        hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) {
            hr = scaler->Initialize(srcBitmap, dstW, dstH, WICBitmapInterpolationModeFant);
        }
        if (FAILED(hr)) {
            if (scaler) scaler->Release();
            srcBitmap->Release();
            err = L"初始化缩放器失败。";
            return false;
        }
        encodeSource = scaler;
    }

    IWICStream* stream = NULL;
    IWICBitmapEncoder* encoder = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    IPropertyBag2* props = NULL;
    IWICColorContext* colorCtx = NULL;

    bool ok = false;
    do {
        hr = factory->CreateStream(&stream);
        if (FAILED(hr)) { err = L"创建输出流失败。"; break; }

        hr = stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) { err = L"无法创建输出文件: " + outputPath; break; }

        hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &encoder);
        if (FAILED(hr)) { err = L"创建 WIC JPEG 编码器失败。"; break; }

        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) { err = L"初始化 JPEG 编码器失败。"; break; }

        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr)) { err = L"创建 JPEG 帧失败。"; break; }

        if (props) {
            PROPBAG2 optQuality = {};
            optQuality.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT varQuality;
            VariantInit(&varQuality);
            varQuality.vt = VT_R4;
            varQuality.fltVal = opt.jpegQuality;
            props->Write(1, &optQuality, &varQuality);
        }

        hr = frame->Initialize(props);
        if (FAILED(hr)) { err = L"初始化 JPEG 帧失败。"; break; }

        if (decoded.data.icc_ptr && decoded.data.icc_len > 0) {
            if (SUCCEEDED(factory->CreateColorContext(&colorCtx))) {
                if (SUCCEEDED(colorCtx->InitializeFromMemory(
                        decoded.data.icc_ptr, (UINT)decoded.data.icc_len))) {
                    frame->SetColorContexts(1, &colorCtx);
                }
            }
        }

        WICPixelFormatGUID outFmt = GUID_WICPixelFormat24bppBGR;
        frame->SetPixelFormat(&outFmt);

        hr = frame->WriteSource(encodeSource, NULL);
        if (FAILED(hr)) { err = L"写入 JPEG 像素失败。"; break; }

        hr = frame->Commit();
        if (FAILED(hr)) { err = L"提交 JPEG 帧失败。"; break; }

        hr = encoder->Commit();
        if (FAILED(hr)) { err = L"完成 JPEG 编码失败。"; break; }

        ok = true;
    } while (false);

    if (colorCtx) colorCtx->Release();
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (scaler) scaler->Release();
    srcBitmap->Release();

    if (!ok) {
        DeleteFileW(outputPath.c_str());
        return false;
    }

    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, outputPath.c_str(), NULL);
    return true;
}

static bool ParseArgs(int argc, LPWSTR* argv, ExportOptions& opt, std::wstring& err) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--tonemap" || a == L"-t") {
            if (i + 1 >= argc) { err = L"缺少 --tonemap 参数。"; return false; }
            std::wstring v = argv[++i];
            if (_wcsicmp(v.c_str(), L"reinhard") == 0) {
                opt.toneMap = HEIF_TONEMAP_REINHARD;
            } else if (_wcsicmp(v.c_str(), L"aces") == 0 || _wcsicmp(v.c_str(), L"aces-filmic") == 0) {
                opt.toneMap = HEIF_TONEMAP_ACES_FILMIC;
            } else {
                err = L"未知的色调映射模式: " + v;
                return false;
            }
        } else if (a == L"--scale" || a == L"-s") {
            if (i + 1 >= argc) { err = L"缺少 --scale 参数。"; return false; }
            int s = _wtoi(argv[++i]);
            if (s != 100 && s != 50 && s != 25) {
                err = L"--scale 仅支持 100、50 或 25。";
                return false;
            }
            opt.scalePercent = (uint32_t)s;
        } else if (a == L"--quality" || a == L"-q") {
            if (i + 1 >= argc) { err = L"缺少 --quality 参数。"; return false; }
            int q = _wtoi(argv[++i]);
            if (q < 10) q = 10;
            if (q > 100) q = 100;
            opt.jpegQuality = (float)q / 100.0f;
        } else if (a == L"--output" || a == L"-o") {
            if (i + 1 >= argc) { err = L"缺少 --output 参数。"; return false; }
            opt.explicitOutput = argv[++i];
        } else if (a == L"--quiet") {
            opt.quiet = true;
        } else if (a == L"--register-menu") {
            opt.registerMenu = true;
        } else if (a == L"--unregister-menu") {
            opt.unregisterMenu = true;
        } else if (!a.empty() && a[0] == L'-') {
            err = L"未知选项: " + a;
            return false;
        } else {
            opt.inputs.push_back(a);
        }
    }
    if (opt.registerMenu || opt.unregisterMenu) {
        return true;
    }
    if (opt.inputs.empty()) {
        err = L"用法: heif_export.exe --tonemap <reinhard|aces> --scale <100|50|25> [--quality 92] [--output out.jpg] <file.HIF>";
        return false;
    }
    if (!opt.explicitOutput.empty() && opt.inputs.size() > 1) {
        err = L"--output 只能用于单个输入文件。";
        return false;
    }
    return true;
}

static LSTATUS SetRegStr(HKEY hKey, const WCHAR* name, const WCHAR* val) {
    DWORD cb = (DWORD)((wcslen(val) + 1) * sizeof(WCHAR));
    return RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE*)val, cb);
}

static LSTATUS SetRegDword(HKEY hKey, const WCHAR* name, DWORD val) {
    return RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
}

static bool RegisterContextMenu(std::wstring& err) {
    WCHAR exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        err = L"获取 heif_export.exe 路径失败。";
        return false;
    }

    struct MenuItemDef {
        const WCHAR* key;
        const WCHAR* label;
        const WCHAR* args;
        bool sepBefore;
    };
    const MenuItemDef items[] = {
        { L"01_reinhard_100", L"Reinhard - \u539F\u56FE (100%)",    L"--tonemap reinhard --scale 100", false },
        { L"02_reinhard_050", L"Reinhard - 50%",                     L"--tonemap reinhard --scale 50",  false },
        { L"03_reinhard_025", L"Reinhard - 25%",                     L"--tonemap reinhard --scale 25",  false },
        { L"04_aces_100",     L"ACES Filmic - \u539F\u56FE (100%)", L"--tonemap aces --scale 100",     true  },
        { L"05_aces_050",     L"ACES Filmic - 50%",                  L"--tonemap aces --scale 50",      false },
        { L"06_aces_025",     L"ACES Filmic - 25%",                  L"--tonemap aces --scale 25",      false },
    };
    const WCHAR* exts[] = { L".hif", L".heif", L".heic" };

    for (const WCHAR* ext : exts) {
        std::wstring rootPath = std::wstring(L"SOFTWARE\\Classes\\SystemFileAssociations\\") + ext + L"\\shell\\SonyHifExportJpg";
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, rootPath.c_str());

        HKEY hRoot = NULL;
        LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, rootPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hRoot, NULL);
        if (s != ERROR_SUCCESS) {
            err = L"写入注册表失败（请使用管理员权限运行）。";
            return false;
        }
        SetRegStr(hRoot, L"MUIVerb", L"\u5BFC\u51FA\u4E3A sRGB JPG");
        SetRegStr(hRoot, L"SubCommands", L"");
        SetRegStr(hRoot, L"Icon", L"imageres.dll,-72");
        RegCloseKey(hRoot);

        for (const auto& it : items) {
            std::wstring itemPath = rootPath + L"\\shell\\" + it.key;
            std::wstring cmdPath = itemPath + L"\\command";

            HKEY hItem = NULL;
            s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, itemPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hItem, NULL);
            if (s != ERROR_SUCCESS) {
                err = L"写入子菜单注册表项失败。";
                return false;
            }
            SetRegStr(hItem, L"MUIVerb", it.label);
            if (it.sepBefore) {
                SetRegDword(hItem, L"CommandFlags", 0x20); // ECF_SEPARATORBEFORE
            }
            RegCloseKey(hItem);

            HKEY hCmd = NULL;
            s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, cmdPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hCmd, NULL);
            if (s != ERROR_SUCCESS) {
                err = L"写入子菜单命令失败。";
                return false;
            }
            std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" " + it.args + L" \"%1\"";
            SetRegStr(hCmd, NULL, cmdLine.c_str());
            RegCloseKey(hCmd);
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return true;
}

static bool UnregisterContextMenu(std::wstring& err) {
    const WCHAR* exts[] = { L".hif", L".heif", L".heic" };
    for (const WCHAR* ext : exts) {
        std::wstring rootPath = std::wstring(L"SOFTWARE\\Classes\\SystemFileAssociations\\") + ext + L"\\shell\\SonyHifExportJpg";
        LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, rootPath.c_str());
        if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND) {
            err = L"删除右键菜单注册表项失败（请使用管理员权限运行）。";
            return false;
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return true;
}

static void ReportError(const std::wstring& msg, bool quiet) {
    if (quiet) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = NULL;
        freopen_s(&f, "CONERR$", "w", stderr);
        fwprintf(stderr, L"[heif_export] %ls\n", msg.c_str());
        FreeConsole();
    } else {
        MessageBoxW(NULL, msg.c_str(), L"Sony HIF 导出 JPG 失败", MB_OK | MB_ICONERROR);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    ExportOptions opt;
    std::wstring err;
    if (!ParseArgs(argc, argv, opt, err)) {
        LocalFree(argv);
        ReportError(err, opt.quiet);
        return 2;
    }
    LocalFree(argv);

    if (opt.registerMenu) {
        std::wstring regErr;
        if (!RegisterContextMenu(regErr)) {
            ReportError(regErr, opt.quiet);
            return 6;
        }
        return 0;
    }
    if (opt.unregisterMenu) {
        std::wstring regErr;
        if (!UnregisterContextMenu(regErr)) {
            ReportError(regErr, opt.quiet);
            return 7;
        }
        return 0;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        ReportError(L"COM 初始化失败。", opt.quiet);
        return 3;
    }

    IWICImagingFactory* factory = NULL;
    hr = CoCreateInstance(
        CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        CoUninitialize();
        ReportError(L"无法创建 WIC 工厂实例。", opt.quiet);
        return 4;
    }

    // Serialize multi-select Explorer launches so Rayon worker pools do not thrash CPU/RAM.
    MutexGuard lock(L"Local\\CustomHeifWicExportMutex");

    int failures = 0;
    std::wstring firstErr;
    for (const auto& inputPath : opt.inputs) {
        std::wstring outputPath = opt.explicitOutput.empty()
            ? BuildDefaultOutputPath(inputPath, opt)
            : opt.explicitOutput;
        std::wstring oneErr;
        if (!ExportOneFile(factory, inputPath, outputPath, opt, oneErr)) {
            failures++;
            if (firstErr.empty()) firstErr = oneErr;
        }
    }

    factory->Release();
    CoUninitialize();

    if (failures > 0) {
        ReportError(firstErr, opt.quiet);
        return 5;
    }
    return 0;
}
