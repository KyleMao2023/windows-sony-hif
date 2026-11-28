#define INITGUID
#include <windows.h>
#include <wincodec.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include <memory>
#include <new>

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

struct HeifWicProbed {
    uint32_t width;
    uint32_t height;
    uint32_t bits_per_channel;
    uint8_t* icc_ptr;
    size_t icc_len;
};

int32_t heif_wic_probe(const uint8_t* input_ptr, size_t input_len, HeifWicProbed* out);
void heif_wic_probe_free(HeifWicProbed* probed);
int32_t heif_wic_decode(const uint8_t* input_ptr, size_t input_len, HeifWicDecoded* out);
void heif_wic_free(HeifWicDecoded* decoded);
}

// {B7E3C420-5F1A-4D9C-8A23-1D2E3F4A5B01}
DEFINE_GUID(CLSID_CustomHeifDecoder,
    0xb7e3c420, 0x5f1a, 0x4d9c, 0x8a, 0x23, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b, 0x01);

// {B7E3C420-5F1A-4D9C-8A23-1D2E3F4A5B02}
DEFINE_GUID(GUID_ContainerFormatCustomHeif,
    0xb7e3c420, 0x5f1a, 0x4d9c, 0x8a, 0x23, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b, 0x02);

// {B7E3C420-5F1A-4D9C-8A23-1D2E3F4A5B03}
DEFINE_GUID(GUID_VendorCustomHeif,
    0xb7e3c420, 0x5f1a, 0x4d9c, 0x8a, 0x23, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b, 0x03);

static HMODULE g_hModule = NULL;
static volatile LONG g_objCount = 0;

class EmptyEnumString : public IEnumString {
    volatile LONG m_ref;
public:
    EmptyEnumString() : m_ref(1) { InterlockedIncrement(&g_objCount); }
    virtual ~EmptyEnumString() { InterlockedDecrement(&g_objCount); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_IEnumString) {
            *ppv = static_cast<IEnumString*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG, LPOLESTR*, ULONG* pceltFetched) override {
        if (pceltFetched) *pceltFetched = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumString** ppEnum) override {
        if (!ppEnum) return E_INVALIDARG;
        *ppEnum = new (std::nothrow) EmptyEnumString();
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }
};

// Minimal non-null IWICMetadataQueryReader so callers like Adobe AIDE.dll that dereference
// GetMetadataQueryReader without checking HRESULT do not null-deref crash.
class HeifMetadataQueryReader : public IWICMetadataQueryReader {
    volatile LONG m_ref;
public:
    HeifMetadataQueryReader() : m_ref(1) { InterlockedIncrement(&g_objCount); }
    virtual ~HeifMetadataQueryReader() { InterlockedDecrement(&g_objCount); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_IWICMetadataQueryReader) {
            *ppv = static_cast<IWICMetadataQueryReader*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetContainerFormat(GUID* pguidContainerFormat) override {
        if (!pguidContainerFormat) return E_INVALIDARG;
        *pguidContainerFormat = GUID_ContainerFormatCustomHeif;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLocation(UINT cchMaxLength, WCHAR* wzNamespace, UINT* pcchActualLength) override {
        if (!pcchActualLength) return E_INVALIDARG;
        *pcchActualLength = 2; // L"/" + NUL
        if (!wzNamespace) return S_OK;
        if (cchMaxLength < 2) return WINCODEC_ERR_INSUFFICIENTBUFFER;
        wzNamespace[0] = L'/';
        wzNamespace[1] = L'\0';
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetadataByName(LPCWSTR wzName, PROPVARIANT* pvarValue) override {
        if (!wzName) return E_INVALIDARG;
        if (pvarValue) PropVariantInit(pvarValue);
        if (_wcsicmp(wzName, L"/ifd/{ushort=274}") == 0 ||
            _wcsicmp(wzName, L"/app1/ifd/{ushort=274}") == 0) {
            if (pvarValue) {
                pvarValue->vt = VT_UI2;
                pvarValue->uiVal = 1; // Orientation already applied during decode
            }
            return S_OK;
        }
        return WINCODEC_ERR_PROPERTYNOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE GetEnumerator(IEnumString** ppIEnumString) override {
        if (!ppIEnumString) return E_INVALIDARG;
        *ppIEnumString = new (std::nothrow) EmptyEnumString();
        return *ppIEnumString ? S_OK : E_OUTOFMEMORY;
    }
};
static volatile LONG g_pinned = 0;
static INIT_ONCE g_pinOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK PinSelfModuleCallback(PINIT_ONCE, PVOID, PVOID*) {
    // Pin the DLL before Rayon initializes its global thread pool, because
    // Rayon worker threads stay parked in WaitOnAddress and crash if unmapped.
    InterlockedExchange(&g_pinned, 1);
    HMODULE hPinned = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&PinSelfModuleCallback),
        &hPinned);
    return TRUE;
}

static void PinSelfModuleOnce() {
    InitOnceExecuteOnce(&g_pinOnce, PinSelfModuleCallback, NULL, NULL);
}

struct LazyImageState {
    CRITICAL_SECTION cs;
    std::vector<uint8_t> raw_bytes;
    HeifWicProbed probed;
    HeifWicDecoded dec;
    bool decoded;
    HRESULT decode_hr;

    LazyImageState() : decoded(false), decode_hr(S_OK) {
        InitializeCriticalSection(&cs);
        memset(&probed, 0, sizeof(probed));
        memset(&dec, 0, sizeof(dec));
    }
    ~LazyImageState() {
        heif_wic_free(&dec);
        heif_wic_probe_free(&probed);
        DeleteCriticalSection(&cs);
    }

    // Defer full HEVC pixel decode until CopyPixels is actually requested so metadata-only
    // callers (e.g. Explorer context menu / PhotoViewer.dll) return in ~1-3 ms.
    HRESULT EnsureDecoded() {
        EnterCriticalSection(&cs);
        if (!decoded) {
            PinSelfModuleOnce();
            int32_t rc = heif_wic_decode(raw_bytes.data(), raw_bytes.size(), &dec);
            std::vector<uint8_t>().swap(raw_bytes);
            decode_hr = (rc == 0 && dec.bgra_ptr) ? S_OK : WINCODEC_ERR_BADIMAGE;
            decoded = true;
        }
        HRESULT hr = decode_hr;
        LeaveCriticalSection(&cs);
        return hr;
    }
};

static bool IsBrand(const uint8_t* b, const char* tag) {
    return memcmp(b, tag, 4) == 0;
}

static bool CheckHeifStreamHeader(IStream* pStream) {
    if (!pStream) return false;
    LARGE_INTEGER zero = {};
    ULARGE_INTEGER oldPos = {};
    if (FAILED(pStream->Seek(zero, STREAM_SEEK_CUR, &oldPos))) return false;
    if (FAILED(pStream->Seek(zero, STREAM_SEEK_SET, NULL))) return false;

    uint8_t buf[64] = {};
    ULONG readBytes = 0;
    HRESULT hr = pStream->Read(buf, sizeof(buf), &readBytes);

    LARGE_INTEGER restorePos;
    restorePos.QuadPart = (LONGLONG)oldPos.QuadPart;
    pStream->Seek(restorePos, STREAM_SEEK_SET, NULL);

    if (FAILED(hr) || readBytes < 12) return false;
    if (!IsBrand(buf + 4, "ftyp")) return false;

    const uint8_t* major = buf + 8;
    if (IsBrand(major, "heic") || IsBrand(major, "heix") ||
        IsBrand(major, "hevc") || IsBrand(major, "hevx")) {
        return true;
    }
    if (IsBrand(major, "mif1") || IsBrand(major, "msf1")) {
        // Avoid hijacking AVIF files that use mif1 as major brand
        uint32_t ftypSize = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
        ULONG limit = (ftypSize < readBytes) ? ftypSize : readBytes;
        for (ULONG off = 16; off + 4 <= limit; off += 4) {
            if (IsBrand(buf + off, "avif") || IsBrand(buf + off, "avis")) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static bool ReadAllStream(IStream* pStream, std::vector<uint8_t>& out) {
    if (!pStream) return false;
    LARGE_INTEGER zero = {};
    if (FAILED(pStream->Seek(zero, STREAM_SEEK_SET, NULL))) return false;

    const size_t MAX_HEIF_BYTES = 256u * 1024u * 1024u;
    out.clear();
    STATSTG st = {};
    if (SUCCEEDED(pStream->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart > 0 && st.cbSize.QuadPart <= MAX_HEIF_BYTES) {
        size_t expected = (size_t)st.cbSize.QuadPart;
        out.resize(expected);
        size_t offset = 0;
        while (offset < expected) {
            ULONG toRead = (ULONG)((expected - offset) > (1024u * 1024u) ? (1024u * 1024u) : (expected - offset));
            ULONG n = 0;
            HRESULT hr = pStream->Read(out.data() + offset, toRead, &n);
            if (FAILED(hr)) return false;
            if (n == 0) break;
            offset += n;
            if (hr == S_FALSE) break;
        }
        out.resize(offset);
        return !out.empty();
    }

    std::vector<uint8_t> chunk(262144);
    while (true) {
        ULONG n = 0;
        HRESULT hr = pStream->Read(chunk.data(), (ULONG)chunk.size(), &n);
        if (FAILED(hr)) return false;
        if (n == 0) break;
        if (out.size() + n > MAX_HEIF_BYTES) return false;
        out.insert(out.end(), chunk.begin(), chunk.begin() + n);
        if (hr == S_FALSE) break;
    }
    return !out.empty();
}

static HRESULT FillColorContextsFromBytes(const uint8_t* icc_ptr, size_t icc_len, UINT cCount, IWICColorContext** ppIColorContexts, UINT* pcActualCount) {
    if (!pcActualCount) return E_INVALIDARG;
    if (!icc_ptr || icc_len == 0) {
        *pcActualCount = 0;
        return S_OK;
    }
    *pcActualCount = 1;
    if (!ppIColorContexts) {
        return S_OK;
    }
    if (cCount < 1) {
        return WINCODEC_ERR_INSUFFICIENTBUFFER;
    }
    if (!ppIColorContexts[0]) {
        return E_INVALIDARG;
    }
    return ppIColorContexts[0]->InitializeFromMemory(icc_ptr, (UINT)icc_len);
}

static HRESULT FillColorContexts(LazyImageState& st, UINT cCount, IWICColorContext** ppIColorContexts, UINT* pcActualCount) {
    EnterCriticalSection(&st.cs);
    const uint8_t* icc_ptr = (st.decoded && SUCCEEDED(st.decode_hr)) ? st.dec.icc_ptr : st.probed.icc_ptr;
    size_t icc_len = (st.decoded && SUCCEEDED(st.decode_hr)) ? st.dec.icc_len : st.probed.icc_len;
    HRESULT hr = FillColorContextsFromBytes(icc_ptr, icc_len, cCount, ppIColorContexts, pcActualCount);
    LeaveCriticalSection(&st.cs);
    return hr;
}

class HeifFrameDecode : public IWICBitmapFrameDecode {
    volatile LONG m_ref;
    std::shared_ptr<LazyImageState> m_state;
public:
    HeifFrameDecode(std::shared_ptr<LazyImageState> state)
        : m_ref(1), m_state(std::move(state)) {
        InterlockedIncrement(&g_objCount);
    }
    virtual ~HeifFrameDecode() {
        InterlockedDecrement(&g_objCount);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_IWICBitmapSource || riid == IID_IWICBitmapFrameDecode) {
            *ppv = static_cast<IWICBitmapFrameDecode*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetSize(UINT* puiWidth, UINT* puiHeight) override {
        if (!puiWidth || !puiHeight) return E_INVALIDARG;
        *puiWidth = m_state->probed.width;
        *puiHeight = m_state->probed.height;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* pPixelFormat) override {
        if (!pPixelFormat) return E_INVALIDARG;
        *pPixelFormat = (m_state->probed.bits_per_channel == 16)
                            ? GUID_WICPixelFormat64bppBGRA
                            : GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetResolution(double* pDpiX, double* pDpiY) override {
        if (!pDpiX || !pDpiY) return E_INVALIDARG;
        *pDpiX = 96.0;
        *pDpiY = 96.0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette*) override {
        return WINCODEC_ERR_PALETTEUNAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* prc, UINT cbStride, UINT cbBufferSize, BYTE* pbBuffer) override {
        if (!pbBuffer) return E_INVALIDARG;
        HRESULT hr = m_state->EnsureDecoded();
        if (FAILED(hr)) return hr;

        const auto& dec = m_state->dec;
        WICRect rc = { 0, 0, (INT)dec.width, (INT)dec.height };
        if (prc) rc = *prc;

        if (rc.X < 0 || rc.Y < 0 || rc.Width <= 0 || rc.Height <= 0 ||
            (UINT)(rc.X + rc.Width) > dec.width || (UINT)(rc.Y + rc.Height) > dec.height) {
            return E_INVALIDARG;
        }

        UINT bpp = (dec.bits_per_channel == 16) ? 8 : 4;
        UINT rowBytes = (UINT)rc.Width * bpp;
        if (cbStride < rowBytes) return E_INVALIDARG;
        UINT requiredSize = (UINT)(rc.Height - 1) * cbStride + rowBytes;
        if (cbBufferSize < requiredSize) return WINCODEC_ERR_INSUFFICIENTBUFFER;

        for (INT dy = 0; dy < rc.Height; ++dy) {
            const BYTE* src = dec.bgra_ptr + (size_t)(rc.Y + dy) * dec.stride + (size_t)rc.X * bpp;
            BYTE* dst = pbBuffer + (size_t)dy * cbStride;
            memcpy(dst, src, rowBytes);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMetadataQueryReader(IWICMetadataQueryReader** ppIMetadataQueryReader) override {
        if (!ppIMetadataQueryReader) return E_INVALIDARG;
        *ppIMetadataQueryReader = new (std::nothrow) HeifMetadataQueryReader();
        return *ppIMetadataQueryReader ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE GetColorContexts(UINT cCount, IWICColorContext** ppIColorContexts, UINT* pcActualCount) override {
        return FillColorContexts(*m_state, cCount, ppIColorContexts, pcActualCount);
    }

    HRESULT STDMETHODCALLTYPE GetThumbnail(IWICBitmapSource** ppIThumbnail) override {
        if (ppIThumbnail) *ppIThumbnail = NULL;
        return WINCODEC_ERR_CODECNOTHUMBNAIL;
    }
};

class HeifDecoder : public IWICBitmapDecoder {
    volatile LONG m_ref;
    CRITICAL_SECTION m_cs;
    std::shared_ptr<LazyImageState> m_state;
public:
    HeifDecoder() : m_ref(1) {
        InitializeCriticalSection(&m_cs);
        InterlockedIncrement(&g_objCount);
    }
    virtual ~HeifDecoder() {
        DeleteCriticalSection(&m_cs);
        InterlockedDecrement(&g_objCount);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_IWICBitmapDecoder) {
            *ppv = static_cast<IWICBitmapDecoder*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE QueryCapability(IStream* pIStream, DWORD* pdwCapability) override {
        if (!pIStream || !pdwCapability) return E_INVALIDARG;
        if (!CheckHeifStreamHeader(pIStream)) {
            *pdwCapability = 0;
            return WINCODEC_ERR_BADHEADER;
        }
        *pdwCapability = WICBitmapDecoderCapabilityCanDecodeAllImages |
                         WICBitmapDecoderCapabilityCanDecodeSomeImages |
                         WICBitmapDecoderCapabilitySameEncoder;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize(IStream* pIStream, WICDecodeOptions) override {
        if (!pIStream) return E_INVALIDARG;
        if (!CheckHeifStreamHeader(pIStream)) {
            return WINCODEC_ERR_BADHEADER;
        }

        auto state = std::make_shared<LazyImageState>();
        if (!ReadAllStream(pIStream, state->raw_bytes)) {
            return WINCODEC_ERR_STREAMREAD;
        }

        int32_t rc = heif_wic_probe(state->raw_bytes.data(), state->raw_bytes.size(), &state->probed);
        if (rc != 0) {
            return WINCODEC_ERR_BADHEADER;
        }

        EnterCriticalSection(&m_cs);
        m_state = std::move(state);
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetContainerFormat(GUID* pguidContainerFormat) override {
        if (!pguidContainerFormat) return E_INVALIDARG;
        *pguidContainerFormat = GUID_ContainerFormatCustomHeif;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDecoderInfo(IWICBitmapDecoderInfo** ppIDecoderInfo) override {
        if (!ppIDecoderInfo) return E_INVALIDARG;
        *ppIDecoderInfo = NULL;
        IWICImagingFactory* factory = NULL;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        IWICComponentInfo* compInfo = NULL;
        hr = factory->CreateComponentInfo(CLSID_CustomHeifDecoder, &compInfo);
        if (SUCCEEDED(hr)) {
            hr = compInfo->QueryInterface(IID_PPV_ARGS(ppIDecoderInfo));
            compInfo->Release();
        }
        factory->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette*) override {
        return WINCODEC_ERR_PALETTEUNAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE GetMetadataQueryReader(IWICMetadataQueryReader** ppIMetadataQueryReader) override {
        if (!ppIMetadataQueryReader) return E_INVALIDARG;
        *ppIMetadataQueryReader = new (std::nothrow) HeifMetadataQueryReader();
        return *ppIMetadataQueryReader ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE GetPreview(IWICBitmapSource** ppIBitmapSource) override {
        if (ppIBitmapSource) *ppIBitmapSource = NULL;
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }

    HRESULT STDMETHODCALLTYPE GetColorContexts(UINT cCount, IWICColorContext** ppIColorContexts, UINT* pcActualCount) override {
        EnterCriticalSection(&m_cs);
        auto state = m_state;
        LeaveCriticalSection(&m_cs);
        if (!state) return WINCODEC_ERR_NOTINITIALIZED;
        return FillColorContexts(*state, cCount, ppIColorContexts, pcActualCount);
    }

    HRESULT STDMETHODCALLTYPE GetThumbnail(IWICBitmapSource** ppIThumbnail) override {
        if (ppIThumbnail) *ppIThumbnail = NULL;
        return WINCODEC_ERR_CODECNOTHUMBNAIL;
    }

    HRESULT STDMETHODCALLTYPE GetFrameCount(UINT* pCount) override {
        if (!pCount) return E_INVALIDARG;
        *pCount = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFrame(UINT index, IWICBitmapFrameDecode** ppIBitmapFrame) override {
        if (!ppIBitmapFrame) return E_INVALIDARG;
        *ppIBitmapFrame = NULL;
        EnterCriticalSection(&m_cs);
        auto state = m_state;
        LeaveCriticalSection(&m_cs);
        if (!state) return WINCODEC_ERR_NOTINITIALIZED;
        if (index != 0) return WINCODEC_ERR_FRAMEMISSING;
        HeifFrameDecode* frame = new (std::nothrow) HeifFrameDecode(std::move(state));
        if (!frame) return E_OUTOFMEMORY;
        *ppIBitmapFrame = frame;
        return S_OK;
    }
};

class HeifClassFactory : public IClassFactory {
    volatile LONG m_ref;
public:
    HeifClassFactory() : m_ref(1) { InterlockedIncrement(&g_objCount); }
    virtual ~HeifClassFactory() { InterlockedDecrement(&g_objCount); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = NULL;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        HeifDecoder* dec = new (std::nothrow) HeifDecoder();
        if (!dec) return E_OUTOFMEMORY;
        HRESULT hr = dec->QueryInterface(riid, ppv);
        dec->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_objCount);
        else InterlockedDecrement(&g_objCount);
        return S_OK;
    }
};

static const WCHAR* CLSID_STR = L"{B7E3C420-5F1A-4D9C-8A23-1D2E3F4A5B01}";
static const WCHAR* CONTAINER_STR = L"{B7E3C420-5F1A-4D9C-8A23-1D2E3F4A5B02}";
static const WCHAR* VENDOR_STR = L"{B7E3C420-5F1A-4D9C-8A23-1D2E3F4A5B03}";
static const WCHAR* CATID_DEC_STR = L"{7ED96837-96F0-4812-B211-F13C24117ED3}";
static const WCHAR* FMT_BGRA_STR = L"{6FDDC324-4E03-4BFE-B185-3D77768DC90F}";
static const WCHAR* FMT_BGRA64_STR = L"{1562FF7C-D352-46F9-979E-42976B792246}";

static HRESULT SetRegStr(HKEY hKey, const WCHAR* name, const WCHAR* val) {
    DWORD cb = (DWORD)((wcslen(val) + 1) * sizeof(WCHAR));
    return HRESULT_FROM_WIN32(RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE*)val, cb));
}

static HRESULT SetRegDword(HKEY hKey, const WCHAR* name, DWORD val) {
    return HRESULT_FROM_WIN32(RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD)));
}

static HRESULT SetRegBin(HKEY hKey, const WCHAR* name, const BYTE* data, DWORD len) {
    return HRESULT_FROM_WIN32(RegSetValueExW(hKey, name, 0, REG_BINARY, data, len));
}

static void WritePattern(HKEY hKey, const WCHAR* sub, const char* brand4) {
    HKEY hPat = NULL;
    if (RegCreateKeyExW(hKey, sub, 0, NULL, 0, KEY_WRITE, NULL, &hPat, NULL) == ERROR_SUCCESS) {
        const BYTE mask12[12] = { 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        BYTE pat[12] = { 0x00, 0x00, 0x00, 0x00, 'f', 't', 'y', 'p',
                         (BYTE)brand4[0], (BYTE)brand4[1], (BYTE)brand4[2], (BYTE)brand4[3] };
        SetRegDword(hPat, L"Position", 0);
        SetRegDword(hPat, L"Length", 12);
        SetRegBin(hPat, L"Pattern", pat, 12);
        SetRegBin(hPat, L"Mask", mask12, 12);
        RegCloseKey(hPat);
    }
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    if (g_pinned != 0) return S_FALSE;
    return (g_objCount == 0) ? S_OK : S_FALSE;
}

HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (rclsid != CLSID_CustomHeifDecoder) return CLASS_E_CLASSNOTAVAILABLE;
    HeifClassFactory* factory = new (std::nothrow) HeifClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT STDAPICALLTYPE DllRegisterServer() {
    WCHAR dllPath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, dllPath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WCHAR keyPath[256];
    HKEY hKey = NULL;

    swprintf(keyPath, 256, L"SOFTWARE\\Classes\\CLSID\\%ls", CLSID_STR);
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);

    SetRegStr(hKey, NULL, L"Custom HEIF WIC Decoder (heic-decoder)");
    SetRegStr(hKey, L"Author", L"Custom HEIF WIC");
    SetRegStr(hKey, L"Description", L"HEIF/HIF 4:2:2 10-bit WIC Decoder powered by heic-decoder");
    SetRegStr(hKey, L"FriendlyName", L"Custom HEIF WIC Decoder");
    SetRegStr(hKey, L"ContainerFormat", CONTAINER_STR);
    SetRegStr(hKey, L"Vendor", VENDOR_STR);
    SetRegStr(hKey, L"FileExtensions", L".heic,.heif,.hif");
    SetRegStr(hKey, L"MimeTypes", L"image/heic,image/heif");
    SetRegStr(hKey, L"Version", L"1.0.0.0");
    SetRegStr(hKey, L"SpecVersion", L"1.0.0.0");
    SetRegStr(hKey, L"ColorManagementVersion", L"1.0.0.0");
    SetRegDword(hKey, L"SupportAnimation", 0);
    SetRegDword(hKey, L"SupportChromaKey", 0);
    SetRegDword(hKey, L"SupportLossless", 1);
    SetRegDword(hKey, L"SupportMultiframe", 0);
    SetRegDword(hKey, L"ArbitrationPriority", 0);

    HKEY hInproc = NULL;
    if (RegCreateKeyExW(hKey, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &hInproc, NULL) == ERROR_SUCCESS) {
        SetRegStr(hInproc, NULL, dllPath);
        SetRegStr(hInproc, L"ThreadingModel", L"Both");
        RegCloseKey(hInproc);
    }

    HKEY hFmt = NULL;
    WCHAR fmtSub[128];
    swprintf(fmtSub, 128, L"Formats\\%ls", FMT_BGRA_STR);
    if (RegCreateKeyExW(hKey, fmtSub, 0, NULL, 0, KEY_WRITE, NULL, &hFmt, NULL) == ERROR_SUCCESS) {
        RegCloseKey(hFmt);
    }
    swprintf(fmtSub, 128, L"Formats\\%ls", FMT_BGRA64_STR);
    if (RegCreateKeyExW(hKey, fmtSub, 0, NULL, 0, KEY_WRITE, NULL, &hFmt, NULL) == ERROR_SUCCESS) {
        RegCloseKey(hFmt);
    }

    WritePattern(hKey, L"Patterns\\0", "heic");
    WritePattern(hKey, L"Patterns\\1", "heix");
    WritePattern(hKey, L"Patterns\\2", "hevc");
    WritePattern(hKey, L"Patterns\\3", "hevx");
    WritePattern(hKey, L"Patterns\\4", "mif1");
    WritePattern(hKey, L"Patterns\\5", "msf1");
    RegCloseKey(hKey);

    swprintf(keyPath, 256, L"SOFTWARE\\Classes\\CLSID\\%ls\\Instance\\%ls", CATID_DEC_STR, CLSID_STR);
    s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    SetRegStr(hKey, L"CLSID", CLSID_STR);
    SetRegStr(hKey, L"FriendlyName", L"Custom HEIF WIC Decoder");
    RegCloseKey(hKey);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return S_OK;
}

HRESULT STDAPICALLTYPE DllUnregisterServer() {
    WCHAR keyPath[256];
    swprintf(keyPath, 256, L"SOFTWARE\\Classes\\CLSID\\%ls\\Instance\\%ls", CATID_DEC_STR, CLSID_STR);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);

    swprintf(keyPath, 256, L"SOFTWARE\\Classes\\CLSID\\%ls", CLSID_STR);
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND) {
        return HRESULT_FROM_WIN32(s);
    }
    return S_OK;
}

} // extern "C"
