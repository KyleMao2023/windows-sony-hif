#define WIN_ENV 1
#define _WINDOWS 1
#define MSWindows 1
#define WIN32 1

#include <windows.h>
#include <icm.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <map>
#include <algorithm>

#include "PITypes.h"
#include "PIGeneral.h"
#include "PIFormat.h"
#include "PIProperties.h"

typedef void (*PFN_PluginMain)(const int16, FormatRecordPtr, intptr_t*, int16*);

struct MockHandle {
    std::vector<uint8_t> bytes;
    void* lockedPtr;
};

static Handle MockNewProc(int32 size) {
    auto* mh = new MockHandle();
    mh->bytes.resize((size_t)size, 0);
    mh->lockedPtr = mh->bytes.data();
    return reinterpret_cast<Handle>(mh);
}
static void MockDisposeProc(Handle h) {
    delete reinterpret_cast<MockHandle*>(h);
}
static Ptr MockLockProc(Handle h, Boolean) {
    auto* mh = reinterpret_cast<MockHandle*>(h);
    mh->lockedPtr = mh->bytes.data();
    return reinterpret_cast<Ptr>(mh->lockedPtr);
}
static void MockUnlockProc(Handle) {}
static int32 MockGetSizeProc(Handle h) {
    return (int32)reinterpret_cast<MockHandle*>(h)->bytes.size();
}
static OSErr MockSetSizeProc(Handle h, int32 newSize) {
    reinterpret_cast<MockHandle*>(h)->bytes.resize((size_t)newSize);
    return noErr;
}

static std::vector<uint8_t> g_capturedExif;
static std::vector<uint8_t> g_capturedXmp;
static std::vector<uint8_t> g_fullImage;
static FormatRecord* g_curFr = nullptr;

static Boolean MockAbortProc() { return FALSE; }
static void MockProgressProc(int32, int32) {}

static OSErr MockAdvanceState() {
    auto* fr = g_curFr;
    int32 w = fr->imageSize32.h;
    int32 top = fr->theRect32.top;
    int32 bottom = fr->theRect32.bottom;
    int32 left = fr->theRect32.left;
    int32 right = fr->theRect32.right;
    if (!fr->data || bottom <= top || right <= left) return noErr;
    size_t rowBytes = (size_t)fr->rowBytes;
    const uint8_t* src = static_cast<const uint8_t*>(fr->data);
    for (int32 y = top; y < bottom; ++y) {
        size_t dstOff = (size_t)y * (size_t)w * (size_t)fr->colBytes;
        memcpy(g_fullImage.data() + dstOff, src + (size_t)(y - top) * rowBytes, (size_t)(right - left) * (size_t)fr->colBytes);
    }
    return noErr;
}

static OSErr MockSetPropertyProc(PIType, PIType key, int32, intptr_t, Handle complexVal) {
    Handle h = complexVal;
    if (!h) return paramErr;
    auto* mh = reinterpret_cast<MockHandle*>(h);
    if (key == propEXIFData) g_capturedExif = mh->bytes;
    if (key == propXMP) g_capturedXmp = mh->bytes;
    return noErr;
}

static void run_case(PFN_PluginMain pMain, const wchar_t* path, uint32_t mode, bool useRevert) {
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("Open failed %ls (err=%lu)\n", path, GetLastError());
        return;
    }

    HandleProcs hp = {};
    hp.handleProcsVersion = 1;
    hp.numHandleProcs = 6;
    hp.newProc = MockNewProc;
    hp.disposeProc = MockDisposeProc;
    hp.getSizeProc = MockGetSizeProc;
    hp.setSizeProc = MockSetSizeProc;
    hp.lockProc = MockLockProc;
    hp.unlockProc = MockUnlockProc;

    PropertyProcs pp = {};
    pp.propertyProcsVersion = 1;
    pp.numPropertyProcs = 2;
    pp.setPropertyProc = MockSetPropertyProc;

    FormatRecord fr = {};
    g_curFr = &fr;
    fr.abortProc = MockAbortProc;
    fr.progressProc = MockProgressProc;
    fr.dataFork = reinterpret_cast<intptr_t>(hFile);
    fr.handleProcs = &hp;
    fr.propertyProcs = &pp;
    fr.advanceState = MockAdvanceState;
    fr.canUseICCProfiles = 1;
    fr.HostSupports32BitCoordinates = 1;
    fr.transparencyPlane = -1;

    intptr_t data = 0;
    int16 res = 0;

    pMain(formatSelectorFilterFile, &fr, &data, &res);
    int16 filterRes = res;

    if (useRevert) {
        struct Rev { uint32_t ver; uint32_t mode; } rev = {1, mode};
        fr.revertInfo = MockNewProc(sizeof(rev));
        memcpy(MockLockProc(fr.revertInfo, FALSE), &rev, sizeof(rev));
    } else {
        fr.openForPreview = TRUE;
    }

    pMain(formatSelectorReadPrepare, &fr, &data, &res);
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    pMain(formatSelectorReadStart, &fr, &data, &res);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    g_capturedExif.clear();
    g_capturedXmp.clear();

    if (res == noErr) {
        size_t pxBytes = (size_t)fr.imageSize32.h * (size_t)fr.imageSize32.v * (size_t)fr.planes * (size_t)((fr.depth + 7) / 8);
        g_fullImage.assign(pxBytes, 0);
        pMain(formatSelectorReadContinue, &fr, &data, &res);
    }

    int16 contRes = res;
    pMain(formatSelectorReadFinish, &fr, &data, &res);
    CloseHandle(hFile);

    BOOL iccValid = FALSE;
    if (fr.iCCprofileData && fr.iCCprofileSize > 0) {
        auto* iccMh = reinterpret_cast<MockHandle*>(fr.iCCprofileData);
        PROFILE prof = { PROFILE_MEMBUFFER, iccMh->bytes.data(), (DWORD)iccMh->bytes.size() };
        HPROFILE hpIcc = OpenColorProfileW(&prof, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
        if (hpIcc) {
            IsColorProfileValid(hpIcc, &iccValid);
            CloseColorProfile(hpIcc);
        }
    }

    printf("%ls [mode=%u%s]: filter=%d start+cont=%d (%.1fms) %dx%d depth=%d mode=%d planes=%d maxVal=%d icc=%dB(valid=%d) exif=%zuB\n",
        wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path,
        mode, useRevert ? "" : " preview",
        filterRes, contRes, ms,
        fr.imageSize32.h, fr.imageSize32.v, fr.depth, fr.imageMode, fr.planes, fr.maxValue,
        fr.iCCprofileSize, iccValid, g_capturedExif.size());

    if (contRes == noErr && !g_fullImage.empty()) {
        size_t x = (size_t)fr.imageSize32.h / 2;
        size_t y = (size_t)fr.imageSize32.v / 10;
        size_t idx = (y * (size_t)fr.imageSize32.h + x) * (size_t)fr.planes;
        if (fr.depth == 32) {
            const float* f = reinterpret_cast<const float*>(g_fullImage.data());
            float maxF = 0.0f;
            size_t totalF = g_fullImage.size() / 4;
            for (size_t i = 0; i < totalF; i += 16) maxF = std::max(maxF, f[i]);
            printf("  Sky(10%%,50%%) lin=(%.4f, %.4f, %.4f) sampledPeak=%.4f\n", f[idx], f[idx+1], f[idx+2], maxF);
        } else {
            const uint16_t* u = reinterpret_cast<const uint16_t*>(g_fullImage.data());
            printf("  Sky(10%%,50%%) u16=(%u, %u, %u)\n", u[idx], u[idx+1], u[idx+2]);
        }
    }

    if (fr.iCCprofileData) MockDisposeProc(fr.iCCprofileData);
    if (fr.revertInfo) MockDisposeProc(fr.revertInfo);
}

int main(int argc, char** argv) {
    HMODULE hMod = LoadLibraryW(L"SonyHifFormat.8bi");
    if (!hMod) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    auto pMain = (PFN_PluginMain)GetProcAddress(hMod, "PluginMain");
    if (!pMain) {
        printf("GetProcAddress(PluginMain) failed\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        wchar_t wp[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, wp, MAX_PATH);
        run_case(pMain, wp, 0, true);
        run_case(pMain, wp, 1, true);
        run_case(pMain, wp, 2, true);
        run_case(pMain, wp, 1, false);
    }
    return 0;
}
