// cpoldcrit.cpp - "Old sword crit" overlay DLL for Minecraft Bedrock (Windows, DirectX 11).
// When you attack while falling after a jump (= a critical hit situation) the whole picture
// dips down a little and comes back, like the old sword-hit bob. Cosmetic only: it changes the
// image that is shown on screen, never the game's memory, packets, hits or damage.
//
// How it works (no game addresses needed, so it survives Minecraft updates):
//   * a worker thread polls oldcrit.cfg (written by the HTMI.proxx button) and the mouse/Space keys
//   * IDXGISwapChain::Present / Present1 are hooked by patching the swap-chain vtable
//   * while a bob is playing, the back buffer is copied and put back shifted down a few pixels
//
// LIMITS (be aware):
//   * It cannot see whether the hit really landed or was a real crit - it triggers on
//     "left click while falling after a jump". Exact detection needs Bedrock addresses.
//   * The HUD / hotbar moves together with the world (it is a screen-space effect).
//   * DirectX 11 only. If the game runs on DX12 / RTX the DLL loads and does nothing.
//
// Build: x64 Native Tools Command Prompt:  see build.bat
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include "cpoldcrit_core.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

static const wchar_t* kCfg = L"C:\\Users\\Public\\CheesyProxxy\\oldcrit.cfg";
static const DWORD    kMagic = 0x5452434F;   // "OCRT"

static std::atomic<bool> g_on{false};
static std::atomic<bool> g_hooked{false};
static cpcrit::CritTracker g_trk;            // written by the input thread, read by Present
static CRITICAL_SECTION  g_cs;
static LARGE_INTEGER     g_freq;

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PresentFn  g_origPresent  = nullptr;
static Present1Fn g_origPresent1 = nullptr;

static double nowSec() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_freq.QuadPart;
}

// ------------------------------------------------------------------ frame shift
static ID3D11Texture2D* g_tmp = nullptr;
static UINT g_tmpW = 0, g_tmpH = 0; static DXGI_FORMAT g_tmpFmt = DXGI_FORMAT_UNKNOWN;
static ID3D11Device* g_tmpDev = nullptr;
static bool g_failed = false;                // set if the copy ever fails: stop trying

static void shiftFrame(IDXGISwapChain* sc, int px) {
    if (g_failed || px <= 0) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) { g_failed = true; return; }
    ID3D11Device* dev = nullptr; back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr; if (dev) dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    if (!dev || !ctx || d.SampleDesc.Count != 1 || (int)d.Height <= px * 4) { g_failed = true; goto done; }
    if (!g_tmp || g_tmpDev != dev || g_tmpW != d.Width || g_tmpH != d.Height || g_tmpFmt != d.Format) {
        if (g_tmp) { g_tmp->Release(); g_tmp = nullptr; }
        D3D11_TEXTURE2D_DESC t = d;
        t.BindFlags = 0; t.MiscFlags = 0; t.CPUAccessFlags = 0; t.Usage = D3D11_USAGE_DEFAULT;
        t.MipLevels = 1; t.ArraySize = 1;
        if (FAILED(dev->CreateTexture2D(&t, nullptr, &g_tmp))) { g_tmp = nullptr; g_failed = true; goto done; }
        g_tmpDev = dev; g_tmpW = d.Width; g_tmpH = d.Height; g_tmpFmt = d.Format;
    }
    ctx->CopyResource(g_tmp, back);                                   // snapshot
    {   D3D11_BOX box = { 0, 0, 0, d.Width, d.Height - (UINT)px, 1 };   // rows 0 .. H-px
        ctx->CopySubresourceRegion(back, 0, 0, (UINT)px, 0, g_tmp, 0, &box); }  // put back lower
done:
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    back->Release();
}

static void applyBob(IDXGISwapChain* sc) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    double t = nowSec();
    EnterCriticalSection(&g_cs);
    bool act = g_trk.active(t); float lv = act ? g_trk.level(t) : 0.0f;
    LeaveCriticalSection(&g_cs);
    if (!act || lv <= 0.0f) return;
    DXGI_SWAP_CHAIN_DESC sd; if (FAILED(sc->GetDesc(&sd))) return;
    int px = cpcrit::shiftPixels(lv, (int)sd.BufferDesc.Height);
    if (px > 0) shiftFrame(sc, px);
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) applyBob(sc);
    return g_origPresent(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!(flags & DXGI_PRESENT_TEST)) applyBob(sc);
    return g_origPresent1(sc, sync, flags, pp);
}

// ------------------------------------------------------------------ vtable hook
static bool patchSlot(void** slot, void* hook, void** orig) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot; *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

static LRESULT CALLBACK dummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static bool installHook() {
    WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = dummyProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CPOldCritDummy"; RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) return false;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *(void***)sc;            // the vtable is shared by every swap chain of this class
        ok = patchSlot(&vt[8], (void*)hkPresent, (void**)&g_origPresent);       // IDXGISwapChain::Present
        IDXGISwapChain1* sc1 = nullptr;
        if (ok && SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            void** vt1 = *(void***)sc1;
            patchSlot(&vt1[22], (void*)hkPresent1, (void**)&g_origPresent1);    // IDXGISwapChain1::Present1
            sc1->Release();
        }
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyWindow(hw); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

// ------------------------------------------------------------------ worker thread
static bool readCfg() {
    HANDLE f = CreateFileW(kCfg, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD buf[2] = {0, 0}, got = 0; ReadFile(f, buf, 8, &got, nullptr); CloseHandle(f);
    return got == 8 && buf[0] == kMagic && buf[1] == 1;
}

static bool gameFocused() {
    HWND fg = GetForegroundWindow(); if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return false;
    CURSORINFO ci = { sizeof(ci) };                 // cursor hidden = in the world, not in a menu
    return GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING);
}

static DWORD WINAPI worker(LPVOID) {
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);
    bool prevSpace = false, prevClick = false;
    int cfgTick = 0, tries = 0;
    for (;;) {
        if (cfgTick-- <= 0) {                        // ~4x per second
            cfgTick = 125;
            bool on = readCfg();
            g_on.store(on);
            if (on && !g_hooked.load() && tries < 5) {
                ++tries;
                if (installHook()) g_hooked.store(true);
            }
        }
        if (g_on.load() && gameFocused()) {
            bool sp = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            bool lc = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            double t = nowSec();
            EnterCriticalSection(&g_cs);
            if (sp && !prevSpace) g_trk.onJump(t);
            if (lc && !prevClick) g_trk.onAttack(t);
            LeaveCriticalSection(&g_cs);
            prevSpace = sp; prevClick = lc;
        } else { prevSpace = prevClick = false; }
        Sleep(2);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
