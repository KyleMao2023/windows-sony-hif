#define INITGUID
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>

static void check_file(IWICImagingFactory* factory, const wchar_t* path) {
    IWICBitmapDecoder* dec = NULL;
    HRESULT hr = factory->CreateDecoderFromFilename(path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    printf("CreateDecoderFromFilename(%ls) hr=0x%08lx\n", path, hr);
    if (SUCCEEDED(hr) && dec) {
        IWICBitmapDecoderInfo* info = NULL;
        if (SUCCEEDED(dec->GetDecoderInfo(&info)) && info) {
            WCHAR name[256] = {};
            UINT len = 0;
            GUID clsid = {};
            WCHAR clsidStr[64] = {};
            info->GetFriendlyName(255, name, &len);
            info->GetCLSID(&clsid);
            StringFromGUID2(clsid, clsidStr, 64);
            printf("  Selected WIC Decoder: %ls | CLSID=%ls\n", name, clsidStr);
            info->Release();
        }
        dec->Release();
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("Usage: verify_heif_wic.exe <file1.HIF> [file2.HEIC ...]\n");
        return 0;
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { printf("Factory failed: 0x%08lx\n", hr); return 1; }

    for (int i = 1; i < argc; ++i) {
        check_file(factory, argv[i]);
    }

    factory->Release();
    CoUninitialize();
    return 0;
}
