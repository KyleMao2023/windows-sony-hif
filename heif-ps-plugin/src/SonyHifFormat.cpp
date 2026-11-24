#define WIN_ENV 1
#define _WINDOWS 1
#define MSWindows 1
#define WIN32 1

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <memory>
#include <new>
#include <vector>

#include "PITypes.h"
#include "PIGeneral.h"
#include "PIFormat.h"
#include "PIAbout.h"
#include "PIProperties.h"
#include "resource.h"

extern "C" {
struct SonyHifProbe {
    uint32_t width;
    uint32_t height;
    uint32_t bit_depth;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint32_t is_hdr;
};

struct SonyHifDecoded {
    uint32_t width;
    uint32_t height;
    uint32_t planes;
    uint32_t depth;
    uint32_t has_alpha;
    uint8_t* pixel_ptr;
    size_t pixel_len;
    uint8_t* icc_ptr;
    size_t icc_len;
    uint8_t* exif_ptr;
    size_t exif_len;
    uint8_t* xmp_ptr;
    size_t xmp_len;
};

int32_t sony_hif_probe(const uint8_t* input_ptr, size_t input_len, SonyHifProbe* out_probe);
int32_t sony_hif_decode(const uint8_t* input_ptr, size_t input_len, uint32_t mode, SonyHifDecoded* out);
void sony_hif_free(SonyHifDecoded* decoded);
}

static constexpr uint32_t MODE_32F_LINEAR_HDR = 0;
static constexpr uint32_t MODE_16U_TONEMAPPED = 1;
static constexpr uint32_t MODE_16U_RAW_SIGNAL = 2;

static HINSTANCE g_hInstance = nullptr;
static INIT_ONCE g_pinOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK PinSelfModuleCallback(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE hPinned = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&PinSelfModuleCallback),
        &hPinned);
    return TRUE;
}

static void PinSelfModuleOnce() {
    InitOnceExecuteOnce(&g_pinOnce, PinSelfModuleCallback, nullptr, nullptr);
}

struct RevertData {
    uint32_t version;
    uint32_t mode;
};

struct PluginGlobals {
    SonyHifDecoded decoded;
    uint32_t mode;
    bool hasDecoded;
};

struct DialogContext {
    SonyHifProbe probe;
    uint32_t selectedMode;
};

static const wchar_t* GetModeDescText(uint32_t mode) {
    switch (mode) {
        case MODE_32F_LINEAR_HDR:
            return L"保留全部 HLG 高光余量（>1.0 线性场景光），附带线性 BT.2020 ICC，进 PS 为 RGB/32* HDR 文档。";
        case MODE_16U_TONEMAPPED:
            return L"自动柔和压缩高光至 sRGB 亮度曲线并保留 BT.2020 广色域，进 PS 为 RGB/16*（内存减半、兼容全滤镜）。";
        default:
            return L"直接按 16 位无损传出原始 10-bit 编码信号（不做 HLG 映射），供专业 LUT 或自定义曲线使用。";
    }
}

static void UpdateDialogSelection(HWND hDlg, uint32_t mode) {
    SetDlgItemTextW(hDlg, IDC_DESC_LABEL, GetModeDescText(mode));
}

static INT_PTR CALLBACK OptionsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, lParam);
            auto* ctx = reinterpret_cast<DialogContext*>(lParam);

            SetWindowTextW(hDlg, L"Sony HIF 导入选项 (10-bit HLG / HEIF)");

            const wchar_t* primariesStr =
                (ctx->probe.color_primaries == 9) ? L"BT.2020" :
                (ctx->probe.color_primaries == 12) ? L"Display P3" : L"sRGB/BT.709";
            const wchar_t* tcStr =
                (ctx->probe.transfer_characteristics == 18) ? L"HLG (HDR)" :
                (ctx->probe.transfer_characteristics == 16) ? L"PQ (HDR)" : L"SDR";

            wchar_t infoBuf[256];
            if (ctx->probe.width > 0 && ctx->probe.height > 0) {
                swprintf(infoBuf, 256, L"图像信息: %u × %u | %u-bit | %ls %ls",
                    ctx->probe.width, ctx->probe.height, ctx->probe.bit_depth, primariesStr, tcStr);
            } else {
                swprintf(infoBuf, 256, L"图像信息: %u-bit | %ls %ls",
                    ctx->probe.bit_depth, primariesStr, tcStr);
            }
            SetDlgItemTextW(hDlg, IDC_INFO_LABEL, infoBuf);

            SetDlgItemTextW(hDlg, IDC_RADIO_32F_HDR,
                L"32 位浮点线性 HDR（RGB/32，完整保留 HLG 高光）[推荐]");
            SetDlgItemTextW(hDlg, IDC_RADIO_16U_TM,
                L"16 位整数 — 高光色调映射（RGB/16，即开即用）");
            SetDlgItemTextW(hDlg, IDC_RADIO_16U_RAW,
                L"16 位整数 — 原始编码直出（RGB/16，不做曲线映射）");
            SetDlgItemTextW(hDlg, IDOK, L"确定打开");
            SetDlgItemTextW(hDlg, IDCANCEL, L"取消");

            int radioId = (ctx->selectedMode == MODE_32F_LINEAR_HDR) ? IDC_RADIO_32F_HDR :
                          (ctx->selectedMode == MODE_16U_TONEMAPPED) ? IDC_RADIO_16U_TM : IDC_RADIO_16U_RAW;
            CheckRadioButton(hDlg, IDC_RADIO_32F_HDR, IDC_RADIO_16U_RAW, radioId);
            UpdateDialogSelection(hDlg, ctx->selectedMode);
            return TRUE;
        }
        case WM_COMMAND: {
            auto* ctx = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
            WORD id = LOWORD(wParam);
            if (id == IDC_RADIO_32F_HDR) {
                if (ctx) ctx->selectedMode = MODE_32F_LINEAR_HDR;
                UpdateDialogSelection(hDlg, MODE_32F_LINEAR_HDR);
                return TRUE;
            }
            if (id == IDC_RADIO_16U_TM) {
                if (ctx) ctx->selectedMode = MODE_16U_TONEMAPPED;
                UpdateDialogSelection(hDlg, MODE_16U_TONEMAPPED);
                return TRUE;
            }
            if (id == IDC_RADIO_16U_RAW) {
                if (ctx) ctx->selectedMode = MODE_16U_RAW_SIGNAL;
                UpdateDialogSelection(hDlg, MODE_16U_RAW_SIGNAL);
                return TRUE;
            }
            if (id == IDOK) {
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

static bool IsBrand(const uint8_t* b, const char* tag) {
    return memcmp(b, tag, 4) == 0;
}

static bool CheckHeifHeaderBytes(const uint8_t* buf, size_t len) {
    if (len < 12 || !IsBrand(buf + 4, "ftyp")) {
        return false;
    }
    const uint8_t* major = buf + 8;
    if (IsBrand(major, "heic") || IsBrand(major, "heix") ||
        IsBrand(major, "hevc") || IsBrand(major, "hevx")) {
        return true;
    }
    if (IsBrand(major, "mif1") || IsBrand(major, "msf1")) {
        uint32_t ftypSize = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
        size_t limit = (ftypSize < len) ? (size_t)ftypSize : len;
        for (size_t off = 16; off + 4 <= limit; off += 4) {
            if (IsBrand(buf + off, "avif") || IsBrand(buf + off, "avis")) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static bool ReadDataForkBytes(intptr_t dataFork, std::vector<uint8_t>& out, size_t maxBytes) {
    HANDLE hFile = reinterpret_cast<HANDLE>(dataFork);
    if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN)) {
        return false;
    }
    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
        return false;
    }
    size_t toRead = (size_t)fileSize.QuadPart;
    if (maxBytes > 0 && maxBytes < toRead) {
        toRead = maxBytes;
    }
    if (toRead > 512ULL * 1024ULL * 1024ULL) {
        return false;
    }
    out.resize(toRead);
    size_t total = 0;
    while (total < toRead) {
        DWORD chunk = (DWORD)std::min<size_t>(toRead - total, 64ULL * 1024ULL * 1024ULL);
        DWORD got = 0;
        if (!ReadFile(hFile, out.data() + total, chunk, &got, nullptr) || got == 0) {
            return false;
        }
        total += got;
    }
    return true;
}

static void SetRectHelper(FormatRecordPtr formatRecord, int32 top, int32 left, int32 bottom, int32 right) {
    if (formatRecord->HostSupports32BitCoordinates && formatRecord->PluginUsing32BitCoordinates) {
        formatRecord->theRect32.top = top;
        formatRecord->theRect32.left = left;
        formatRecord->theRect32.bottom = bottom;
        formatRecord->theRect32.right = right;
    }
    formatRecord->theRect.top = (int16)std::clamp<int32>(top, -32768, 32767);
    formatRecord->theRect.left = (int16)std::clamp<int32>(left, -32768, 32767);
    formatRecord->theRect.bottom = (int16)std::clamp<int32>(bottom, -32768, 32767);
    formatRecord->theRect.right = (int16)std::clamp<int32>(right, -32768, 32767);
}

static Handle AllocAndCopyHandle(HandleProcs* handleProcs, const uint8_t* src, size_t len) {
    if (!handleProcs || !handleProcs->newProc || !handleProcs->lockProc || !handleProcs->unlockProc ||
        !src || len == 0 || len > 0x7fffffffULL) {
        return nullptr;
    }
    Handle h = handleProcs->newProc((int32)len);
    if (!h) return nullptr;
    Ptr p = handleProcs->lockProc(h, FALSE);
    if (!p) {
        if (handleProcs->disposeProc) handleProcs->disposeProc(h);
        return nullptr;
    }
    memcpy(p, src, len);
    handleProcs->unlockProc(h);
    return h;
}

static int16 DoFilterFile(FormatRecordPtr formatRecord) {
    std::vector<uint8_t> hdr;
    if (!ReadDataForkBytes(formatRecord->dataFork, hdr, 64)) {
        return formatCannotRead;
    }
    return CheckHeifHeaderBytes(hdr.data(), hdr.size()) ? noErr : formatCannotRead;
}

static int16 DoReadStart(FormatRecordPtr formatRecord, PluginGlobals* globals) {
    globals->hasDecoded = false;
    memset(&globals->decoded, 0, sizeof(globals->decoded));

    std::vector<uint8_t> fileBytes;
    if (!ReadDataForkBytes(formatRecord->dataFork, fileBytes, 0)) {
        return readErr;
    }
    if (!CheckHeifHeaderBytes(fileBytes.data(), std::min<size_t>(fileBytes.size(), 64))) {
        return formatCannotRead;
    }

    SonyHifProbe probe = {};
    sony_hif_probe(fileBytes.data(), fileBytes.size(), &probe);

    uint32_t chosenMode = probe.is_hdr ? MODE_32F_LINEAR_HDR : MODE_16U_TONEMAPPED;
    bool haveSavedMode = false;

    if (formatRecord->revertInfo && formatRecord->handleProcs &&
        formatRecord->handleProcs->lockProc && formatRecord->handleProcs->unlockProc &&
        formatRecord->handleProcs->getSizeProc) {
        if (formatRecord->handleProcs->getSizeProc(formatRecord->revertInfo) >= (int32)sizeof(RevertData)) {
            auto* rev = reinterpret_cast<RevertData*>(formatRecord->handleProcs->lockProc(formatRecord->revertInfo, FALSE));
            if (rev && rev->version == 1 && rev->mode <= MODE_16U_RAW_SIGNAL) {
                chosenMode = rev->mode;
                haveSavedMode = true;
            }
            formatRecord->handleProcs->unlockProc(formatRecord->revertInfo);
        }
    }

    if (!haveSavedMode) {
        if (formatRecord->openForPreview) {
            chosenMode = MODE_16U_TONEMAPPED;
        } else {
            HWND hwndOwner = nullptr;
            if (formatRecord->platformData) {
                hwndOwner = reinterpret_cast<HWND>(
                    reinterpret_cast<PlatformData*>(formatRecord->platformData)->hwnd);
            }
            if (!hwndOwner) hwndOwner = GetActiveWindow();

            DialogContext dlgCtx = { probe, chosenMode };
            INT_PTR res = DialogBoxParamW(
                g_hInstance,
                MAKEINTRESOURCEW(IDD_HIF_OPTIONS),
                hwndOwner,
                OptionsDlgProc,
                reinterpret_cast<LPARAM>(&dlgCtx));
            if (res != IDOK) {
                return userCanceledErr;
            }
            chosenMode = dlgCtx.selectedMode;

            if (formatRecord->handleProcs && formatRecord->handleProcs->newProc) {
                RevertData rev = { 1, chosenMode };
                Handle hRev = AllocAndCopyHandle(
                    formatRecord->handleProcs,
                    reinterpret_cast<const uint8_t*>(&rev),
                    sizeof(rev));
                if (hRev) {
                    formatRecord->revertInfo = hRev;
                }
            }
        }
    }

    PinSelfModuleOnce();
    int32_t rc = sony_hif_decode(fileBytes.data(), fileBytes.size(), chosenMode, &globals->decoded);
    if (rc != 0 || !globals->decoded.pixel_ptr) {
        sony_hif_free(&globals->decoded);
        return formatCannotRead;
    }

    globals->mode = chosenMode;
    globals->hasDecoded = true;
    const auto& dec = globals->decoded;

    formatRecord->imageMode = (dec.depth == 32) ? plugInModeRGBColor : plugInModeRGB48;
    formatRecord->depth = (int16)dec.depth;
    formatRecord->planes = (int16)dec.planes;
    formatRecord->imageHRes = (300 << 16);
    formatRecord->imageVRes = (300 << 16);
    if (dec.depth == 16) {
        formatRecord->maxValue = 0x8000;
    }
    if (dec.has_alpha && formatRecord->transparencyPlane != 0) {
        formatRecord->transparencyPlane = (int32)(dec.planes - 1);
        formatRecord->transparencyMatting = 0;
    }

    if (formatRecord->HostSupports32BitCoordinates && formatRecord->PluginUsing32BitCoordinates) {
        formatRecord->imageSize32.h = (int32)dec.width;
        formatRecord->imageSize32.v = (int32)dec.height;
    }
    if (dec.width <= 32767 && dec.height <= 32767) {
        formatRecord->imageSize.h = (int16)dec.width;
        formatRecord->imageSize.v = (int16)dec.height;
    } else if (!formatRecord->PluginUsing32BitCoordinates) {
        sony_hif_free(&globals->decoded);
        globals->hasDecoded = false;
        return formatCannotRead;
    }

    return noErr;
}

static int16 DoReadContinue(FormatRecordPtr formatRecord, PluginGlobals* globals) {
    if (!globals->hasDecoded || !formatRecord->advanceState) {
        return formatBadParameters;
    }
    const auto& dec = globals->decoded;
    int16 planeBytes = (dec.depth == 32) ? 4 : 2;
    int16 colBytes = (int16)(dec.planes * planeBytes);
    size_t rowBytes64 = (size_t)dec.width * (size_t)colBytes;
    if (rowBytes64 > 0x7fffffffULL) {
        return memFullErr;
    }
    int32 rowBytes = (int32)rowBytes64;

    formatRecord->loPlane = 0;
    formatRecord->hiPlane = (int16)(dec.planes - 1);
    formatRecord->planeBytes = planeBytes;
    formatRecord->colBytes = colBytes;
    formatRecord->rowBytes = rowBytes;
    for (int i = 0; i < 16; ++i) {
        formatRecord->planeMap[i] = (int16)i;
    }

    const int32 totalRows = (int32)dec.height;
    const int32 stripRows = 128;
    int16 err = noErr;

    for (int32 y = 0; y < totalRows && err == noErr; y += stripRows) {
        if (formatRecord->abortProc && formatRecord->abortProc()) {
            err = userCanceledErr;
            break;
        }
        int32 rows = std::min(stripRows, totalRows - y);
        SetRectHelper(formatRecord, y, 0, y + rows, (int32)dec.width);
        formatRecord->data = dec.pixel_ptr + (size_t)y * rowBytes64;

        err = formatRecord->advanceState();
        if (formatRecord->progressProc) {
            formatRecord->progressProc(y + rows, totalRows);
        }
    }

    SetRectHelper(formatRecord, 0, 0, 0, 0);
    formatRecord->data = nullptr;
    if (err != noErr) {
        return err;
    }

    // Attach ICC Profile (Photoshop takes ownership of iCCprofileData handle).
    if (formatRecord->canUseICCProfiles && dec.icc_ptr && dec.icc_len > 0 && formatRecord->handleProcs) {
        Handle hIcc = AllocAndCopyHandle(formatRecord->handleProcs, dec.icc_ptr, dec.icc_len);
        if (hIcc) {
            formatRecord->iCCprofileData = hIcc;
            formatRecord->iCCprofileSize = (int32)dec.icc_len;
        }
    }

    // Attach EXIF & XMP metadata (plug-in retains and disposes property handle).
    if (formatRecord->propertyProcs && formatRecord->propertyProcs->setPropertyProc &&
        formatRecord->handleProcs && formatRecord->handleProcs->disposeProc) {
        if (dec.exif_ptr && dec.exif_len > 0) {
            Handle hExif = AllocAndCopyHandle(formatRecord->handleProcs, dec.exif_ptr, dec.exif_len);
            if (hExif) {
                formatRecord->propertyProcs->setPropertyProc(
                    kPhotoshopSignature, propEXIFData, 0, 0, hExif);
                formatRecord->handleProcs->disposeProc(hExif);
            }
        }
        if (dec.xmp_ptr && dec.xmp_len > 0) {
            Handle hXmp = AllocAndCopyHandle(formatRecord->handleProcs, dec.xmp_ptr, dec.xmp_len);
            if (hXmp) {
                formatRecord->propertyProcs->setPropertyProc(
                    kPhotoshopSignature, propXMP, 0, 0, hXmp);
                formatRecord->handleProcs->disposeProc(hXmp);
            }
        }
    }

    return noErr;
}

static int16 DoReadFinish(PluginGlobals* globals) {
    if (globals->hasDecoded) {
        sony_hif_free(&globals->decoded);
        globals->hasDecoded = false;
    }
    return noErr;
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInstance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

__declspec(dllexport) void PluginMain(
    const int16 selector,
    FormatRecordPtr formatRecord,
    intptr_t* data,
    int16* result)
{
    if (!result) return;
    if (selector == formatSelectorAbout) {
        auto* about = reinterpret_cast<AboutRecordPtr>(formatRecord);
        HWND hwnd = (about && about->platformData)
                        ? reinterpret_cast<HWND>(reinterpret_cast<PlatformData*>(about->platformData)->hwnd)
                        : GetActiveWindow();
        MessageBoxW(
            hwnd,
            L"Sony HIF Format Plug-in (10-bit HLG / 4:2:2 HEIF)\n"
            L"支持 32 位浮点线性 HDR（BT.2020）与 16 位高精度导入，附带 ICC 与 EXIF 透传。",
            L"关于 Sony HIF 插件",
            MB_OK | MB_ICONINFORMATION);
        *result = noErr;
        return;
    }

    if (!formatRecord) {
        *result = formatBadParameters;
        return;
    }

    if (formatRecord->HostSupports32BitCoordinates) {
        formatRecord->PluginUsing32BitCoordinates = TRUE;
    }

    switch (selector) {
        case formatSelectorFilterFile:
            *result = DoFilterFile(formatRecord);
            break;
        case formatSelectorReadPrepare:
            formatRecord->maxData = 0;
            *result = noErr;
            break;
        case formatSelectorReadStart: {
            auto* globals = new (std::nothrow) PluginGlobals();
            if (!globals) {
                *result = memFullErr;
                break;
            }
            memset(globals, 0, sizeof(PluginGlobals));
            if (data) *data = reinterpret_cast<intptr_t>(globals);
            *result = DoReadStart(formatRecord, globals);
            if (*result != noErr) {
                DoReadFinish(globals);
                delete globals;
                if (data) *data = 0;
            }
            break;
        }
        case formatSelectorReadContinue: {
            auto* globals = data ? reinterpret_cast<PluginGlobals*>(*data) : nullptr;
            *result = globals ? DoReadContinue(formatRecord, globals) : formatBadParameters;
            break;
        }
        case formatSelectorReadFinish: {
            auto* globals = data ? reinterpret_cast<PluginGlobals*>(*data) : nullptr;
            if (globals) {
                *result = DoReadFinish(globals);
                delete globals;
                if (data) *data = 0;
            } else {
                *result = noErr;
            }
            break;
        }
        default:
            *result = formatBadParameters;
            break;
    }
}

} // extern "C"
