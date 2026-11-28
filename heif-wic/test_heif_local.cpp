#define INITGUID
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <stdio.h>
#include <vector>

DEFINE_GUID(CLSID_CustomHeifDecoder,
    0xb7e3c420, 0x5f1a, 0x4d9c, 0x8a, 0x23, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b, 0x01);

typedef HRESULT (STDAPICALLTYPE *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);

static void run_case(IClassFactory* cf, const wchar_t* path) {
    IWICBitmapDecoder* dec = NULL;
    HRESULT hr = cf->CreateInstance(NULL, IID_PPV_ARGS(&dec));
    if (FAILED(hr)) { printf("CreateInstance failed: 0x%08lx\n", hr); return; }

    IStream* stm = NULL;
    hr = SHCreateStreamOnFileW(path, STGM_READ | STGM_SHARE_DENY_NONE, &stm);
    if (FAILED(hr)) { printf("Open %ls failed: 0x%08lx\n", path, hr); dec->Release(); return; }

    DWORD cap = 0;
    hr = dec->QueryCapability(stm, &cap);
    printf("QueryCapability(%ls): hr=0x%08lx, cap=0x%lx\n", path, hr, cap);

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    hr = dec->Initialize(stm, WICDecodeMetadataCacheOnDemand);
    QueryPerformanceCounter(&t1);
    stm->Release();
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    printf("Initialize(%ls): hr=0x%08lx (%.1f ms)\n", path, hr, ms);

    if (SUCCEEDED(hr)) {
        IWICBitmapFrameDecode* frame = NULL;
        if (SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            WICPixelFormatGUID fmt = {};
            frame->GetPixelFormat(&fmt);
            bool is64 = (fmt == GUID_WICPixelFormat64bppBGRA);
            UINT bpp = is64 ? 8 : 4;

            UINT iccCnt = 0, iccBytes = 0;
            frame->GetColorContexts(0, NULL, &iccCnt);
            if (iccCnt > 0) {
                IWICImagingFactory* fac = NULL;
                CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
                IWICColorContext* cc = NULL;
                if (fac && SUCCEEDED(fac->CreateColorContext(&cc))) {
                    UINT act = 0;
                    if (SUCCEEDED(frame->GetColorContexts(1, &cc, &act))) {
                        cc->GetProfileBytes(0, NULL, &iccBytes);
                    }
                    cc->Release();
                }
                if (fac) fac->Release();
            }

            std::vector<BYTE> row10(w * bpp);
            WICRect rc10 = {0, (INT)(h / 10), (INT)w, 1};
            LARGE_INTEGER cp0, cp1;
            QueryPerformanceCounter(&cp0);
            HRESULT hrCp = frame->CopyPixels(&rc10, w * bpp, (UINT)row10.size(), row10.data());
            QueryPerformanceCounter(&cp1);
            double cpMs = (double)(cp1.QuadPart - cp0.QuadPart) * 1000.0 / (double)freq.QuadPart;
            size_t mid = (w / 2) * bpp;

            HRESULT hrConv = S_OK;
            if (is64) {
                IWICImagingFactory* fac = NULL;
                CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
                IWICFormatConverter* conv = NULL;
                if (fac && SUCCEEDED(fac->CreateFormatConverter(&conv))) {
                    hrConv = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                    conv->Release();
                }
                if (fac) fac->Release();
            }

            IWICMetadataQueryReader* mqr = NULL;
            HRESULT hrMq = frame->GetMetadataQueryReader(&mqr);
            PROPVARIANT pv; PropVariantInit(&pv);
            if (mqr) mqr->GetMetadataByName(L"/ifd/{ushort=274}", &pv);

            if (is64) {
                const UINT16* px = (const UINT16*)(row10.data() + mid);
                printf("  Size=%ux%u, fmt=64bppBGRA(16bit), ICC=%u(%uB), CopyPixels=0x%08lx(%.1f ms), Conv32=0x%08lx, RGB16(10%%,50%%)=(%u,%u,%u), MQR=0x%08lx Ori=%u\n",
                       w, h, iccCnt, iccBytes, hrCp, cpMs, hrConv, px[2], px[1], px[0], hrMq, (unsigned)pv.uiVal);
            } else {
                printf("  Size=%ux%u, fmt=32bppBGRA(8bit), ICC=%u(%uB), CopyPixels=0x%08lx(%.1f ms), RGB8(10%%,50%%)=(%u,%u,%u), MQR=0x%08lx Ori=%u\n",
                       w, h, iccCnt, iccBytes, hrCp, cpMs, row10[mid+2], row10[mid+1], row10[mid], hrMq, (unsigned)pv.uiVal);
            }
            PropVariantClear(&pv);
            if (mqr) mqr->Release();
            frame->Release();
        }
    }
    dec->Release();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("Usage: test_heif_local.exe <file1.HIF> [file2.HEIC ...]\n");
        return 0;
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HMODULE hMod = LoadLibraryW(L"heif_wic.dll");
    if (!hMod) { printf("LoadLibraryW(heif_wic.dll) failed: %lu\n", GetLastError()); return 1; }
    auto pGetClassObj = (PFN_DllGetClassObject)GetProcAddress(hMod, "DllGetClassObject");
    IClassFactory* cf = NULL;
    pGetClassObj(CLSID_CustomHeifDecoder, IID_PPV_ARGS(&cf));

    for (int i = 1; i < argc; ++i) run_case(cf, argv[i]);

    cf->Release();
    FreeLibrary(hMod);
    CoUninitialize();
    return 0;
}
