#include "ui/app.h"
#include "ui/theme.h"
#include <d2d1.h>
#include <dwrite.h>
#include "signaling/signaling_client.h"
#include "signaling/user_id.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "render/gl_renderer.h"
#include "webrtc/webrtc_session.h"
#include "input/input_handler.h"
#include "input/input_injector.h"
#include "core/logger.h"
#include "render/gl_proc.h"

#include <GL/gl.h>
#include <windowsx.h>  // GET_X_LPARAM
#include <objbase.h>   // CoInitializeEx
#include <chrono>
#include <cstring>
#include <algorithm>

namespace omnidesk {

// ---- Win32 WndProc trampoline ----
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (app) return app->handleMessage(hwnd, msg, wp, lp);
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK GlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Forward input events to parent asynchronously (non-blocking)
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
            PostMessage(GetParent(hwnd), msg, wp, lp);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---- App lifecycle ----

App::App() = default;
App::~App() { shutdown(); }

bool App::init(const AppConfig& config) {
    config_ = config;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QueryPerformanceFrequency(&perfFreq_);
    QueryPerformanceCounter(&lastFrame_);

    wakeEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (!createMainWindow()) return false;
    if (!createGlChildWindow()) return false;
    initD2D();

    // Generate or load user ID
    UserIdGenerator idGen;
    myId_ = idGen.loadOrGenerate();
    LOG_INFO("User ID: %s", myId_.id.c_str());

    initSignaling();
    appRunning_ = true;
    tryConnectSignaling();
    startReconnectThread();

    ShowWindow(mainHwnd_, SW_SHOWMAXIMIZED);
    InvalidateRect(mainHwnd_, nullptr, FALSE);
    UpdateWindow(mainHwnd_);

    // Timer created only when text input is focused (see WM_LBUTTONDOWN)

    return true;
}

bool App::createMainWindow() {
    // DPI awareness (loaded dynamically — MinGW may not have the declaration)
    {
        auto hUser32 = GetModuleHandleA("user32.dll");
        if (hUser32) {
            using SetDpiFunc = BOOL(WINAPI*)(void*);
            auto fn = reinterpret_cast<SetDpiFunc>(
                GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
            if (fn) fn(reinterpret_cast<void*>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint everything ourselves
    wc.lpszClassName = L"OmniDesk24";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Scale window size for DPI
    HDC screenDC = GetDC(nullptr);
    int dpiX = GetDeviceCaps(screenDC, LOGPIXELSX);
    ReleaseDC(nullptr, screenDC);
    int scaledW = MulDiv(config_.windowWidth, dpiX, 96);
    int scaledH = MulDiv(config_.windowHeight, dpiX, 96);

    // Use 70% of screen for restore size, centered
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = screenW * 70 / 100;
    int winH = screenH * 70 / 100;
    int winX = (screenW - winW) / 2;
    int winY = (screenH - winH) / 2;

    mainHwnd_ = CreateWindowExW(
        0, L"OmniDesk24", L"OmniDesk24",
        WS_OVERLAPPEDWINDOW,
        winX, winY,
        winW, winH,
        nullptr, nullptr, GetModuleHandle(nullptr),
        this  // passed to WM_CREATE -> GWLP_USERDATA
    );

    if (!mainHwnd_) {
        LOG_ERROR("Failed to create main window");
        return false;
    }
    return true;
}

bool App::createGlChildWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = GlWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"OmniDeskGL";
    RegisterClassExW(&wc);

    glHwnd_ = CreateWindowExW(
        0, L"OmniDeskGL", L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, config_.windowWidth, config_.windowHeight,
        mainHwnd_, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!glHwnd_) {
        LOG_ERROR("Failed to create GL child window");
        return false;
    }

    glDC_ = GetDC(glHwnd_);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 0;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(glDC_, &pfd);
    if (!pf || !SetPixelFormat(glDC_, pf, &pfd)) {
        LOG_ERROR("Failed to set GL pixel format");
        return false;
    }

    // Create a temporary context to load wglCreateContextAttribsARB
    HGLRC tempRC = wglCreateContext(glDC_);
    if (!tempRC) {
        LOG_ERROR("Failed to create temp GL context");
        return false;
    }
    wglMakeCurrent(glDC_, tempRC);

    // Load wglCreateContextAttribsARB
    typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
    auto wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        wglGetProcAddress("wglCreateContextAttribsARB"));

    if (wglCreateContextAttribsARB) {
        // Request OpenGL 3.3 Core Profile
        int attribs[] = {
            0x2091, 3,  // WGL_CONTEXT_MAJOR_VERSION_ARB
            0x2092, 3,  // WGL_CONTEXT_MINOR_VERSION_ARB
            0x9126, 1,  // WGL_CONTEXT_PROFILE_MASK_ARB = CORE_PROFILE_BIT
            0
        };
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);

        glRC_ = wglCreateContextAttribsARB(glDC_, nullptr, attribs);
        if (!glRC_) {
            LOG_ERROR("Failed to create GL 3.3 core context");
            return false;
        }
    } else {
        // Fallback: use the basic context
        wglMakeCurrent(nullptr, nullptr);
        glRC_ = tempRC;
    }

    wglMakeCurrent(glDC_, glRC_);
    loadGLProcs();

    // Enable VSync to cap frame rate and reduce GPU usage
    typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int);
    auto wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
        wglGetProcAddress("wglSwapIntervalEXT"));
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(1);

    wglMakeCurrent(nullptr, nullptr);

    return true;
}

// D2D objects (module-level, created lazily)
static ID2D1Factory* g_d2dFactory = nullptr;
static ID2D1HwndRenderTarget* g_d2dRT = nullptr;
static ID2D1SolidColorBrush* g_d2dBrush = nullptr;
static IDWriteFactory* g_dwFactory = nullptr;
static IDWriteTextFormat* g_fmtNormal = nullptr;
static IDWriteTextFormat* g_fmtBold = nullptr;
static IDWriteTextFormat* g_fmtSmall = nullptr;
static IDWriteTextFormat* g_fmtLarge = nullptr;
static IDWriteTextFormat* g_fmtXL = nullptr;

static void initDWriteFormats() {
    static const IID IID_IDWriteFactory = {
        0xb859ee5a, 0xd838, 0x4b5b,
        {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}
    };
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, IID_IDWriteFactory,
                        reinterpret_cast<IUnknown**>(&g_dwFactory));
    if (!g_dwFactory) return;
    g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15, L"", &g_fmtNormal);
    g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15, L"", &g_fmtBold);
    g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"", &g_fmtSmall);
    g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22, L"", &g_fmtLarge);
    g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 26, L"", &g_fmtXL);
    // Set vertical centering on all
    if (g_fmtNormal) g_fmtNormal->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_fmtBold)   g_fmtBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_fmtSmall)  g_fmtSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_fmtLarge)  g_fmtLarge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_fmtXL)     g_fmtXL->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

static bool ensureD2D(HWND hwnd) {
    if (!g_d2dFactory) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
        if (FAILED(hr) || !g_d2dFactory) return false;
        // DWrite init deferred — do it here
        initDWriteFormats();
    }
    if (!g_d2dRT) {
        RECT rc; GetClientRect(hwnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return false;
        auto sz = D2D1::SizeU(rc.right, rc.bottom);
        g_d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, sz),
            &g_d2dRT);
        if (g_d2dRT) {
            g_d2dRT->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_d2dBrush);
        }
    }
    return g_d2dRT != nullptr;
}

// Helper macros for D2D drawing
// Helper: check if mouse (in DIP coords) is inside a rect
static bool hitRect(float mx, float my, float rx, float ry, float rw, float rh) {
    return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
}

#define D2D_FILL_RECT(x,y,w,h,col) do { g_d2dBrush->SetColor(col); \
    g_d2dRT->FillRectangle(D2D1::RectF(x,y,(x)+(w),(y)+(h)), g_d2dBrush); } while(0)
#define D2D_FILL_RRECT(x,y,w,h,r,col) do { g_d2dBrush->SetColor(col); \
    g_d2dRT->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,(x)+(w),(y)+(h)),r,r), g_d2dBrush); } while(0)
#define D2D_STROKE_RRECT(x,y,w,h,r,col) do { g_d2dBrush->SetColor(col); \
    g_d2dRT->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,(x)+(w),(y)+(h)),r,r), g_d2dBrush); } while(0)
#define D2D_TEXT(txt,x,y,w,h,col,fmt) do { g_d2dBrush->SetColor(col); \
    g_d2dRT->DrawText(txt, (UINT32)wcslen(txt), fmt, D2D1::RectF(x,y,(x)+(w),(y)+(h)), g_d2dBrush); } while(0)

void App::initD2D() {
    // D2D objects created lazily in ensureD2D()
}

void App::showGlWindow() {
    RECT rc;
    GetClientRect(mainHwnd_, &rc);
    MoveWindow(glHwnd_, 0, 0, rc.right, rc.bottom, TRUE);
    ShowWindow(glHwnd_, SW_SHOW);
    wglMakeCurrent(glDC_, glRC_);
}

void App::hideGlWindow() {
    wglMakeCurrent(nullptr, nullptr);
    ShowWindow(glHwnd_, SW_HIDE);
}

void App::invalidate() {
    InvalidateRect(mainHwnd_, nullptr, FALSE);
}

void App::queueAction(std::function<void()> action) {
    {
        std::lock_guard<std::mutex> lock(pendingActionsMutex_);
        pendingActions_.push_back(std::move(action));
    }
    SetEvent(wakeEvent_);
    PostMessage(mainHwnd_, WM_APP, 0, 0); // wake the message loop
}

void App::drainPendingActions() {
    std::vector<std::function<void()>> toRun;
    {
        std::lock_guard<std::mutex> lock(pendingActionsMutex_);
        toRun.swap(pendingActions_);
    }
    for (auto& action : toRun) action();
    if (!toRun.empty()) invalidate();
}

// ---- Main loop ----

void App::run() {
    MSG msg;
    while (true) {
        // Wait for messages or wake event (blocks — 0% CPU when idle)
        // Timeout: viewer needs fast polling for video frames,
        // connecting needs periodic updates for animated dots,
        // dashboard/host can block indefinitely.
        // During viewer session: poll with short sleep when idle
        if (state_ == AppState::SESSION_VIEWER) {
            // Process pending messages (non-blocking)
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            drainPendingActions();
        } else {
            // Non-viewer: block efficiently
            DWORD timeout = (state_ == AppState::CONNECTING) ? 500 : INFINITE;
            MsgWaitForMultipleObjects(1, &wakeEvent_, FALSE, timeout, QS_ALLINPUT);
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            drainPendingActions();
        }

        // Delta time
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastFrame_.QuadPart)
                   / static_cast<float>(perfFreq_.QuadPart);
        lastFrame_ = now;

        // Connection timeout
        if (state_ == AppState::CONNECTING) {
            connectTimeoutSec_ += dt;
            if (connectTimeoutSec_ > 30.0f) {
                connectTimeoutSec_ = 0.0f;
                statusMessage_ = "Connection timed out";
                state_ = AppState::DASHBOARD;
                LOG_WARN("Connect attempt timed out after 30 seconds");
            }
            invalidate(); // animated dots
        }

        // Status message auto-clear
        if (!statusMessage_.empty()) {
            statusMessageTimer_ += dt;
            if (statusMessageTimer_ > 5.0f) {
                statusMessage_.clear();
                statusMessageTimer_ = 0.0f;
                invalidate();
            }
        } else {
            statusMessageTimer_ = 0.0f;
        }

        // State transitions
        if (state_ != prevState_) {
            if (state_ == AppState::SESSION_VIEWER) {
                showGlWindow();
            } else if (prevState_ == AppState::SESSION_VIEWER) {
                hideGlWindow();
            }
            prevState_ = state_;
            invalidate();
        }

        // Viewer: upload frames and render only when needed
        if (state_ == AppState::SESSION_VIEWER) {
            wglMakeCurrent(glDC_, glRC_);

            bool newFrame = false;
            if (viewerSession_) {
                viewerSession_->processOnGlThread();
                auto* renderer = viewerSession_->renderer();
                if (renderer) {
                    RECT rc;
                    GetClientRect(glHwnd_, &rc);
                    newFrame = renderer->render(rc.right, rc.bottom);
                }
            }

            if (newFrame) {
                SwapBuffers(glDC_);
            } else {
                // No new frame — sleep to avoid busy spinning GPU
                Sleep(2);
            }
        }
    }
}

// ---- WM_PAINT handler ----

void App::paint() {
    if (state_ == AppState::SESSION_VIEWER) return;
    if (!ensureD2D(mainHwnd_)) return;

    // Use D2D's logical (DIP) size, not physical pixels from GetClientRect
    D2D1_SIZE_F sz = g_d2dRT->GetSize();
    float W = sz.width;
    float H = sz.height;
    if (W <= 0 || H <= 0) return;

    // Colors
    const auto cBg      = D2D1::ColorF(0.09f, 0.09f, 0.11f);
    const auto cCard    = D2D1::ColorF(0.13f, 0.13f, 0.16f);
    const auto cSurface = D2D1::ColorF(0.16f, 0.16f, 0.20f);
    const auto cAccent  = D2D1::ColorF(0.26f, 0.52f, 0.96f);
    const auto cGreen   = D2D1::ColorF(0.25f, 0.78f, 0.42f);
    const auto cRed     = D2D1::ColorF(0.90f, 0.30f, 0.30f);
    const auto cText    = D2D1::ColorF(0.94f, 0.95f, 0.96f);
    const auto cMuted   = D2D1::ColorF(0.55f, 0.56f, 0.60f);
    const auto cBtn     = D2D1::ColorF(0.18f, 0.18f, 0.22f);
    const auto cBorder  = D2D1::ColorF(0.22f, 0.22f, 0.28f, 0.5f);
    const auto cAmber   = D2D1::ColorF(0.95f, 0.70f, 0.20f);

    // Convert mouse to DIP coords for hover detection
    float dpiX, dpiY;
    g_d2dRT->GetDpi(&dpiX, &dpiY);
    float mx = mouseX_ * 96.0f / dpiX;
    float my = mouseY_ * 96.0f / dpiY;

    g_d2dRT->BeginDraw();
    g_d2dRT->Clear(cBg);

    // ========== Custom Title Bar ==========
    {
        float tbH = kTitleBarH;
        float btnW = 46;

        // Title bar background
        D2D_FILL_RECT(0, 0, W, tbH, D2D1::ColorF(0.11f, 0.11f, 0.14f));

        // App title text
        if (g_fmtSmall) {
            g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D_TEXT(L"  OmniDesk24", 0, 0, 200, tbH, D2D1::ColorF(0.60f, 0.61f, 0.65f), g_fmtSmall);
        }

        // Min / Max / Close buttons (right-aligned)
        float bx = W - btnW * 3;

        // Minimize button
        auto minBg = (hoveredTitleBtn_ == 1) ? D2D1::ColorF(0.22f, 0.22f, 0.28f) : D2D1::ColorF(0, 0, 0, 0);
        D2D_FILL_RECT(bx, 0, btnW, tbH, minBg);
        g_d2dBrush->SetColor(D2D1::ColorF(0.70f, 0.71f, 0.75f));
        float iconM = 16;
        g_d2dRT->DrawLine(D2D1::Point2F(bx + iconM, tbH / 2),
                          D2D1::Point2F(bx + btnW - iconM, tbH / 2), g_d2dBrush, 1.0f);

        // Maximize/Restore button
        bx += btnW;
        auto maxBg = (hoveredTitleBtn_ == 2) ? D2D1::ColorF(0.22f, 0.22f, 0.28f) : D2D1::ColorF(0, 0, 0, 0);
        D2D_FILL_RECT(bx, 0, btnW, tbH, maxBg);
        g_d2dBrush->SetColor(D2D1::ColorF(0.70f, 0.71f, 0.75f));
        if (IsZoomed(mainHwnd_)) {
            // Restore icon: two overlapping squares (front + back)
            float cx2 = bx + btnW / 2;
            float cy2 = tbH / 2;
            float s = 4.0f;
            // Back square (offset top-right)
            g_d2dRT->DrawRectangle(D2D1::RectF(cx2-s+2, cy2-s-2, cx2+s+2, cy2+s-2), g_d2dBrush, 1.0f);
            // Front square (offset bottom-left) — fill bg first to cover back square overlap
            auto tbBg = (hoveredTitleBtn_ == 2) ? D2D1::ColorF(0.22f, 0.22f, 0.28f) : D2D1::ColorF(0.11f, 0.11f, 0.14f);
            D2D_FILL_RECT(cx2-s-1, cy2-s+1, s*2+1, s*2+1, tbBg);
            g_d2dBrush->SetColor(D2D1::ColorF(0.70f, 0.71f, 0.75f));
            g_d2dRT->DrawRectangle(D2D1::RectF(cx2-s-1, cy2-s+1, cx2+s-1, cy2+s+1), g_d2dBrush, 1.0f);
        } else {
            // Maximize icon: single square
            float s = 5;
            g_d2dRT->DrawRectangle(D2D1::RectF(bx+btnW/2-s, tbH/2-s, bx+btnW/2+s, tbH/2+s), g_d2dBrush, 1.0f);
        }

        // Close button
        bx += btnW;
        auto closeBg = (hoveredTitleBtn_ == 3) ? D2D1::ColorF(0.90f, 0.18f, 0.18f) : D2D1::ColorF(0, 0, 0, 0);
        D2D_FILL_RECT(bx, 0, btnW, tbH, closeBg);
        auto xCol = (hoveredTitleBtn_ == 3) ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0.70f, 0.71f, 0.75f);
        g_d2dBrush->SetColor(xCol);
        float xm = 15, ym = 10;
        g_d2dRT->DrawLine(D2D1::Point2F(bx+xm, ym), D2D1::Point2F(bx+btnW-xm, tbH-ym), g_d2dBrush, 1.0f);
        g_d2dRT->DrawLine(D2D1::Point2F(bx+btnW-xm, ym), D2D1::Point2F(bx+xm, tbH-ym), g_d2dBrush, 1.0f);

        // Subtle separator line below title bar
        D2D_FILL_RECT(0, tbH - 0.5f, W, 0.5f, D2D1::ColorF(0.18f, 0.18f, 0.22f, 0.6f));
    }

    if (state_ == AppState::DASHBOARD) {
        // Centered card
        float cardW = W * 0.35f;
        if (cardW < 380) cardW = 380;
        if (cardW > 520) cardW = 520;
        float cardH = 280;
        float cx = (W - cardW) / 2;
        float cy = H * 0.38f - cardH / 2;
        if (cy < kTitleBarH + 20) cy = kTitleBarH + 20;
        float pad = 24;
        float innerW = cardW - pad * 2;

        D2D_FILL_RRECT(cx, cy, cardW, cardH, 14, cCard);

        float y = cy + pad;

        // Title
        if (g_fmtLarge) {
            g_fmtLarge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"OmniDesk24", cx, y, cardW, 30, cAccent, g_fmtLarge);
        }
        y += 36;

        // Status pill
        bool online = signaling_ && signaling_->isConnected();
        const wchar_t* pill = online ? L"Online" : L"Offline";
        float pillW = 72, pillH = 22;
        float pillX = cx + (cardW - pillW) / 2;
        D2D_FILL_RRECT(pillX, y, pillW, pillH, 11, online ? cGreen : cRed);
        if (g_fmtSmall) {
            g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(pill, pillX, y, pillW, pillH, cText, g_fmtSmall);
        }
        y += 36;

        // "Your Device ID"
        float lx = cx + pad;
        if (g_fmtSmall) {
            g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D_TEXT(L"Your Device ID", lx, y, innerW, 20, cMuted, g_fmtSmall);
        }
        y += 24;

        // ID box
        float idBoxH = 48;
        D2D_FILL_RRECT(lx, y, innerW, idBoxH, 8, cSurface);
        std::wstring wid(myId_.id.begin(), myId_.id.end());
        if (g_fmtXL) {
            g_fmtXL->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(wid.c_str(), lx, y, innerW - 70, idBoxH, cText, g_fmtXL);
        }
        // Copy button
        float copyX = lx + innerW - 60, copyY = y + 9;
        rCopyBtn_ = {copyX, copyY, 52, 30};
        bool copyHov = hitRect(mx, my, copyX, copyY, 52, 30);
        D2D_FILL_RRECT(copyX, copyY, 52, 30, 4, copyHov ? D2D1::ColorF(0.28f,0.28f,0.33f) : cBtn);
        if (g_fmtSmall) {
            g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Copy", copyX, copyY, 52, 30, cText, g_fmtSmall);
        }
        y += idBoxH + 20;

        // "Connect to Remote Desktop"
        if (g_fmtSmall) {
            g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D_TEXT(L"Connect to Remote Desktop", lx, y, innerW, 20, cMuted, g_fmtSmall);
        }
        y += 24;

        // Input + Connect button
        float btnW = 90, inputW2 = innerW - btnW - 10, inputH = 38;
        rConnectInput_ = {lx, y, inputW2, inputH};
        bool inputHov = hitRect(mx, my, lx, y, inputW2, inputH);
        D2D_FILL_RRECT(lx, y, inputW2, inputH, 6, inputHov ? D2D1::ColorF(0.19f,0.19f,0.24f) : cSurface);
        if (connectInputFocused_) {
            D2D_STROKE_RRECT(lx, y, inputW2, inputH, 6, cAccent);
        } else if (inputHov) {
            D2D_STROKE_RRECT(lx, y, inputW2, inputH, 6, D2D1::ColorF(0.30f,0.30f,0.38f,0.7f));
        } else {
            D2D_STROKE_RRECT(lx, y, inputW2, inputH, 6, cBorder);
        }
        std::wstring wconn(connectIdInput_.begin(), connectIdInput_.end());
        if (g_fmtNormal) {
            g_fmtNormal->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            if (wconn.empty() && !connectInputFocused_) {
                D2D_TEXT(L"  Enter ID...", lx, y, inputW2, inputH, cMuted, g_fmtNormal);
            } else {
                D2D_TEXT(wconn.c_str(), lx + 10, y, inputW2 - 12, inputH, cText, g_fmtNormal);
                if (connectInputFocused_ && ((GetTickCount() / 530) % 2 == 0)) {
                    float cursorX = lx + 10;
                    if (g_dwFactory && !wconn.empty()) {
                        IDWriteTextLayout* layout = nullptr;
                        g_dwFactory->CreateTextLayout(wconn.c_str(), (UINT32)wconn.size(),
                            g_fmtNormal, inputW2, inputH, &layout);
                        if (layout) {
                            DWRITE_TEXT_METRICS metrics;
                            layout->GetMetrics(&metrics);
                            cursorX += metrics.width;
                            layout->Release();
                        }
                    }
                    g_d2dBrush->SetColor(cText);
                    g_d2dRT->DrawLine(D2D1::Point2F(cursorX, y + 8),
                                       D2D1::Point2F(cursorX, y + inputH - 8), g_d2dBrush, 1.5f);
                }
            }
        }
        // Connect button
        float cbx = lx + inputW2 + 10;
        rConnectBtn_ = {cbx, y, btnW, inputH};
        bool connHov = hitRect(mx, my, cbx, y, btnW, inputH);
        D2D_FILL_RRECT(cbx, y, btnW, inputH, 6, connHov ? D2D1::ColorF(0.34f,0.58f,0.98f) : cAccent);
        if (g_fmtBold) {
            g_fmtBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Connect", cbx, y, btnW, inputH, cText, g_fmtBold);
        }
        y += inputH + 10;

        // Status message
        if (!statusMessage_.empty()) {
            std::wstring ws(statusMessage_.begin(), statusMessage_.end());
            if (g_fmtSmall) {
                g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                D2D_TEXT(ws.c_str(), lx, y, innerW, 20, cAmber, g_fmtSmall);
            }
        }

        // Recent Connections card
        float rcY = cy + cardH + 16;
        float rcH = 80;
        D2D_FILL_RRECT(cx, rcY, cardW, rcH, 10, cCard);
        if (g_fmtBold) {
            g_fmtBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D_TEXT(L"Recent Connections", cx + pad, rcY + 12, innerW, 20, cText, g_fmtBold);
        }
        if (g_fmtSmall) {
            g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"No recent connections", cx + pad, rcY + 42, innerW, 20, cMuted, g_fmtSmall);
        }
    }
    else if (state_ == AppState::CONNECTING) {
        float cardW = 320, cardH = 150;
        float cx = (W - cardW) / 2, cy = (H - cardH) / 2;
        D2D_FILL_RRECT(cx, cy, cardW, cardH, 12, cCard);
        if (g_fmtNormal) {
            g_fmtNormal->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Connecting to", cx, cy + 16, cardW, 24, cMuted, g_fmtNormal);
        }
        int dots = (GetTickCount() / 500) % 4;
        std::wstring target(connectIdInput_.begin(), connectIdInput_.end());
        for (int i = 0; i < dots; i++) target += L".";
        if (g_fmtLarge) {
            g_fmtLarge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(target.c_str(), cx, cy + 44, cardW, 30, cAccent, g_fmtLarge);
        }
        D2D_FILL_RRECT(cx + 20, cy + cardH - 50, cardW - 40, 34, 6, cBtn);
        if (g_fmtNormal) {
            g_fmtNormal->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Cancel", cx + 20, cy + cardH - 50, cardW - 40, 34, cText, g_fmtNormal);
        }
    }
    else if (state_ == AppState::SESSION_HOST) {
        if (g_fmtLarge) {
            g_fmtLarge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Hosting Session", 0, H * 0.40f, W, 30, cAccent, g_fmtLarge);
        }
        if (g_fmtNormal) {
            g_fmtNormal->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"A viewer is connected to your desktop.", 0, H * 0.40f + 36, W, 20, cMuted, g_fmtNormal);
        }
    }

    // ========== Connection Dialog Overlay ==========
    if (showConnectionDialog_) {
        // Dim background
        D2D_FILL_RECT(0, 0, W, H, D2D1::ColorF(0, 0, 0, 0.55f));

        float dlgW = 380, dlgH = 220;
        float dx = (W - dlgW) / 2, dy = (H - dlgH) / 2;
        D2D_FILL_RRECT(dx, dy, dlgW, dlgH, 14, cCard);

        if (g_fmtBold) {
            g_fmtBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Incoming Connection", dx, dy + 24, dlgW, 24, cText, g_fmtBold);
        }

        // Requester ID box
        std::wstring fromId(pendingConnectionFrom_.id.begin(), pendingConnectionFrom_.id.end());
        D2D_FILL_RRECT(dx + 40, dy + 64, dlgW - 80, 42, 6, cSurface);
        if (g_fmtLarge) {
            g_fmtLarge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(fromId.c_str(), dx + 40, dy + 64, dlgW - 80, 42, cAccent, g_fmtLarge);
        }

        if (g_fmtSmall) {
            g_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"wants to view your desktop", dx, dy + 118, dlgW, 20, cMuted, g_fmtSmall);
        }

        // Accept / Reject buttons
        float btnW2 = 130, btnGap = 16;
        float totalBW = btnW2 * 2 + btnGap;
        float btnX = dx + (dlgW - totalBW) / 2;
        float btnY2 = dy + dlgH - 56;

        bool accHov = hitRect(mx, my, btnX, btnY2, btnW2, 38);
        D2D_FILL_RRECT(btnX, btnY2, btnW2, 38, 6, accHov ? D2D1::ColorF(0.30f,0.85f,0.50f) : cGreen);
        if (g_fmtBold) {
            g_fmtBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Accept", btnX, btnY2, btnW2, 38, cText, g_fmtBold);
        }

        float rejX = btnX + btnW2 + btnGap;
        bool rejHov = hitRect(mx, my, rejX, btnY2, btnW2, 38);
        D2D_FILL_RRECT(rejX, btnY2, btnW2, 38, 6, rejHov ? D2D1::ColorF(0.95f,0.35f,0.35f) : cRed);
        if (g_fmtBold) {
            g_fmtBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D_TEXT(L"Reject", rejX, btnY2, btnW2, 38, cText, g_fmtBold);
        }
    }

    HRESULT hr = g_d2dRT->EndDraw();
    if (hr == static_cast<HRESULT>(0x8899000C)) {
        if (g_d2dBrush) { g_d2dBrush->Release(); g_d2dBrush = nullptr; }
        g_d2dRT->Release(); g_d2dRT = nullptr;
    }
}

// ---- Win32 message handler ----

LRESULT App::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            paint();
            ValidateRect(hwnd, nullptr); // D2D renders directly — no BeginPaint/EndPaint
            return 0;
        }
        case WM_SIZE: {
            UINT w2 = LOWORD(lp), h2 = HIWORD(lp);
            if (w2 > 0 && h2 > 0) {
                if (g_d2dRT) {
                    HRESULT hr = g_d2dRT->Resize(D2D1::SizeU(w2, h2));
                    if (FAILED(hr)) {
                        if (g_d2dBrush) { g_d2dBrush->Release(); g_d2dBrush = nullptr; }
                        g_d2dRT->Release(); g_d2dRT = nullptr;
                    }
                }
                if (glHwnd_ && IsWindowVisible(glHwnd_)) {
                    MoveWindow(glHwnd_, 0, 0, w2, h2, TRUE);
                }
                // Force immediate repaint (no waiting for message loop)
                paint();
                ValidateRect(hwnd, nullptr);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            mouseX_ = static_cast<float>(GET_X_LPARAM(lp));
            mouseY_ = static_cast<float>(GET_Y_LPARAM(lp));
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER) {
                inputHandler_->onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            } else {
                // Detect hover changes — only repaint when something changes
                float dpix = 96.0f, dpiy = 96.0f;
                if (g_d2dRT) g_d2dRT->GetDpi(&dpix, &dpiy);

                // Title bar buttons hover
                float tbHP = kTitleBarH * dpix / 96.0f;
                float btnWP = 46.0f * dpix / 96.0f;
                RECT rc; GetClientRect(mainHwnd_, &rc);
                float rw = static_cast<float>(rc.right);
                int newTitleHov = 0;
                if (mouseY_ < tbHP && mouseX_ > rw - btnWP * 3) {
                    if (mouseX_ > rw - btnWP) newTitleHov = 3;
                    else if (mouseX_ > rw - btnWP * 2) newTitleHov = 2;
                    else newTitleHov = 1;
                }

                // Dashboard element hover (in DIP coords)
                float hmx = mouseX_ * 96.0f / dpix;
                float hmy = mouseY_ * 96.0f / dpiy;
                int newElemHov = 0;
                if (hitRect(hmx, hmy, rCopyBtn_.x, rCopyBtn_.y, rCopyBtn_.w, rCopyBtn_.h)) newElemHov = 1;
                else if (hitRect(hmx, hmy, rConnectBtn_.x, rConnectBtn_.y, rConnectBtn_.w, rConnectBtn_.h)) newElemHov = 2;
                else if (hitRect(hmx, hmy, rConnectInput_.x, rConnectInput_.y, rConnectInput_.w, rConnectInput_.h)) newElemHov = 3;

                static int prevElemHov = 0;
                if (newTitleHov != hoveredTitleBtn_ || newElemHov != prevElemHov) {
                    hoveredTitleBtn_ = newTitleHov;
                    prevElemHov = newElemHov;
                    invalidate();
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float mx = static_cast<float>(GET_X_LPARAM(lp));
            float my = static_cast<float>(GET_Y_LPARAM(lp));

            // Title bar buttons (in physical pixels)
            {
                float dpix = 96.0f;
                if (g_d2dRT) { float _; g_d2dRT->GetDpi(&dpix, &_); }
                float tbH = kTitleBarH * dpix / 96.0f;
                float btnW = 46.0f * dpix / 96.0f;
                RECT rc; GetClientRect(mainHwnd_, &rc);
                float rw = static_cast<float>(rc.right);

                if (my < tbH && mx > rw - btnW * 3) {
                    if (mx > rw - btnW) {
                        // Close
                        PostMessage(hwnd, WM_CLOSE, 0, 0);
                        return 0;
                    } else if (mx > rw - btnW * 2) {
                        // Maximize / Restore
                        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                        return 0;
                    } else {
                        // Minimize
                        ShowWindow(hwnd, SW_MINIMIZE);
                        return 0;
                    }
                }
            }

            if (inputHandler_ && state_ == AppState::SESSION_VIEWER) {
                inputHandler_->onMouseButton(0, true);
                return 0;
            }

            // Dashboard hit testing
            if (state_ == AppState::DASHBOARD) {
                // Convert mouse to DIP
                float dpx = 96.0f, dpy = 96.0f;
                if (g_d2dRT) g_d2dRT->GetDpi(&dpx, &dpy);
                float hmx = mx * 96.0f / dpx;
                float hmy = my * 96.0f / dpy;

                if (hitRect(hmx, hmy, rConnectBtn_.x, rConnectBtn_.y, rConnectBtn_.w, rConnectBtn_.h)) {
                    connectToPeer(connectIdInput_);
                } else if (hitRect(hmx, hmy, rCopyBtn_.x, rCopyBtn_.y, rCopyBtn_.w, rCopyBtn_.h)) {
                    if (OpenClipboard(mainHwnd_)) {
                        EmptyClipboard();
                        size_t len = myId_.id.size() + 1;
                        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len);
                        if (hg) {
                            memcpy(GlobalLock(hg), myId_.id.c_str(), len);
                            GlobalUnlock(hg);
                            SetClipboardData(CF_TEXT, hg);
                        }
                        CloseClipboard();
                        statusMessage_ = "ID copied to clipboard";
                    }
                } else if (hitRect(hmx, hmy, rSettingsBtn_.x, rSettingsBtn_.y, rSettingsBtn_.w, rSettingsBtn_.h)) {
                    showSettings_ = !showSettings_;
                } else if (hitRect(hmx, hmy, rConnectInput_.x, rConnectInput_.y, rConnectInput_.w, rConnectInput_.h)) {
                    connectInputFocused_ = true;
                    SetTimer(mainHwnd_, 1, 530, nullptr); // start cursor blink
                } else {
                    connectInputFocused_ = false;
                    KillTimer(mainHwnd_, 1); // stop cursor blink
                }
                invalidate();
            }

            // Convert click to DIP for D2D hit testing
            float dipMx = mx, dipMy = my;
            if (g_d2dRT) {
                float dpx2, dpy2;
                g_d2dRT->GetDpi(&dpx2, &dpy2);
                dipMx = mx * 96.0f / dpx2;
                dipMy = my * 96.0f / dpy2;
            }

            // Connecting cancel button
            if (state_ == AppState::CONNECTING) {
                D2D1_SIZE_F sz = g_d2dRT ? g_d2dRT->GetSize() : D2D1::SizeF(1280, 800);
                float w = sz.width, h = sz.height;
                float cardW = 320, cardH = 150;
                float cx = (w - cardW) / 2, cy = (h - cardH) / 2;
                if (dipMx >= cx + 20 && dipMx <= cx + cardW - 20 &&
                    dipMy >= cy + cardH - 50 && dipMy <= cy + cardH - 16) {
                    state_ = AppState::DASHBOARD;
                    invalidate();
                }
            }

            // Connection dialog buttons
            if (showConnectionDialog_) {
                D2D1_SIZE_F sz = g_d2dRT ? g_d2dRT->GetSize() : D2D1::SizeF(1280, 800);
                float w = sz.width, h = sz.height;
                float dlgW = 380, dlgH = 220;
                float dx = (w - dlgW) / 2, dy = (h - dlgH) / 2;
                float btnW2 = 130, btnY2 = dy + dlgH - 56;
                float gap = 16;
                float totalBW = btnW2 * 2 + gap;
                float btnX2 = dx + (dlgW - totalBW) / 2;

                // Accept
                if (dipMx >= btnX2 && dipMx <= btnX2 + btnW2 && dipMy >= btnY2 && dipMy <= btnY2 + 38) {
                    showConnectionDialog_ = false;
                    // Accept connection (start host session)
                    hostSession_ = std::make_unique<HostSession>();
                    if (hostSession_->start(config_.encoder, config_.capture)) {
                        if (webrtcSession_) {
                            webrtcSession_->setOnDisconnected(nullptr);
                            webrtcSession_->close();
                            webrtcSession_.reset();
                        }
                        WebRtcConfig wrtcCfg;
                        wrtcCfg.turnServer = config_.turnServer;
                        wrtcCfg.turnUser = config_.turnUser;
                        wrtcCfg.turnPassword = config_.turnPassword;
                        webrtcSession_ = std::make_unique<WebRtcSession>(
                            signaling_.get(), pendingConnectionFrom_, wrtcCfg);
                        hostSession_->setSendCallback([this](const EncodedPacket& pkt) {
                            if (webrtcSession_) webrtcSession_->sendVideo(pkt.data.data(), pkt.data.size());
                        });
                        webrtcSession_->setOnConnected([this]() {
                            queueAction([this]() {
                                state_ = AppState::SESSION_HOST;
                                if (hostSession_) hostSession_->requestKeyFrame();
                            });
                        });
                        webrtcSession_->setOnDisconnected([this]() {
                            queueAction([this]() { disconnectSession(); });
                        });
                        inputInjector_ = std::make_unique<InputInjector>();
                        inputInjector_->setScreenSize(config_.encoder.width, config_.encoder.height);
                        webrtcSession_->setOnData([this](const uint8_t* data, size_t size) {
                            if (size >= 1 + InputEvent::SIZE && data[0] == 0x01) {
                                InputEvent ev = InputEvent::deserialize(data + 1);
                                if (inputInjector_) inputInjector_->inject(ev);
                            }
                        });
                        signaling_->acceptConnection(pendingConnectionFrom_);
                        webrtcSession_->startAsHost();
                    } else {
                        statusMessage_ = "Failed to start host session";
                        signaling_->rejectConnection(pendingConnectionFrom_);
                    }
                    invalidate();
                }
                // Reject
                float rejX2 = btnX2 + btnW2 + gap;
                if (dipMx >= rejX2 && dipMx <= rejX2 + btnW2 &&
                    dipMy >= btnY2 && dipMy <= btnY2 + 38) {
                    showConnectionDialog_ = false;
                    signaling_->rejectConnection(pendingConnectionFrom_);
                    invalidate();
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER) {
                inputHandler_->onMouseButton(0, false);
            }
            return 0;
        }
        case WM_RBUTTONDOWN:
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER)
                inputHandler_->onMouseButton(1, true);
            return 0;
        case WM_RBUTTONUP:
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER)
                inputHandler_->onMouseButton(1, false);
            return 0;
        case WM_MBUTTONDOWN:
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER)
                inputHandler_->onMouseButton(2, true);
            return 0;
        case WM_MBUTTONUP:
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER)
                inputHandler_->onMouseButton(2, false);
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER) {
                inputHandler_->onScroll(delta / 120);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER) {
                inputHandler_->onKey(static_cast<UINT>(lp >> 16) & 0xFF, true);
                return 0;
            }
            // Ctrl+V paste into connect input
            if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && connectInputFocused_ && state_ == AppState::DASHBOARD) {
                if (OpenClipboard(mainHwnd_)) {
                    HANDLE hData = GetClipboardData(CF_TEXT);
                    if (hData) {
                        const char* text = static_cast<const char*>(GlobalLock(hData));
                        if (text) {
                            for (int i = 0; text[i] && connectIdInput_.size() < 8; i++) {
                                char c = static_cast<char>(toupper(static_cast<unsigned char>(text[i])));
                                if (c >= '!' && c <= '~' && c != ' ') connectIdInput_ += c;
                            }
                            GlobalUnlock(hData);
                        }
                    }
                    CloseClipboard();
                    invalidate();
                }
                return 0;
            }
            // Escape to disconnect during session
            if (wp == VK_ESCAPE && (state_ == AppState::SESSION_HOST || state_ == AppState::SESSION_VIEWER)) {
                disconnectSession();
                invalidate();
                return 0;
            }
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (inputHandler_ && state_ == AppState::SESSION_VIEWER) {
                inputHandler_->onKey(static_cast<UINT>(lp >> 16) & 0xFF, false);
            }
            return 0;
        }
        case WM_CHAR: {
            // Text input for connect ID field
            if (connectInputFocused_ && state_ == AppState::DASHBOARD) {
                wchar_t ch = static_cast<wchar_t>(wp);
                if (ch == 8) { // backspace
                    if (!connectIdInput_.empty()) connectIdInput_.pop_back();
                } else if (ch >= 32 && connectIdInput_.size() < 8) {
                    // Uppercase only
                    char c = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
                    if (c != ' ') connectIdInput_ += c;
                } else if (ch == 13) { // Enter
                    connectToPeer(connectIdInput_);
                }
                invalidate();
            }
            return 0;
        }
        case WM_TIMER:
            if (connectInputFocused_) invalidate(); // blink cursor
            return 0;
        // Title bar button clicks/hover handled in WM_LBUTTONDOWN/WM_MOUSEMOVE
        case WM_APP: // cross-thread wake-up
            return 0;
        case WM_NCCALCSIZE: {
            if (wp == TRUE && IsZoomed(hwnd)) {
                // When maximized, Windows extends the window beyond screen edges
                // by the invisible border size. Compensate by insetting the client area.
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                int frame = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left   += frame;
                p->rgrc[0].top    += frame;
                p->rgrc[0].right  -= frame;
                p->rgrc[0].bottom -= frame;
            }
            return 0;
        }
        case WM_NCHITTEST: {
            // Custom hit testing for resize borders and title bar
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            RECT rc; GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom;
            int border = 6; // resize border thickness

            // Resize edges
            bool top = pt.y < border;
            bool bottom = pt.y > h - border;
            bool left = pt.x < border;
            bool right = pt.x > w - border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;

            // Title bar area (top kTitleBarH pixels, scaled for DPI)
            float dpix = 96.0f;
            if (g_d2dRT) { float _; g_d2dRT->GetDpi(&dpix, &_); }
            int titleH = static_cast<int>(kTitleBarH * dpix / 96.0f);

            if (pt.y < titleH) {
                // Title bar buttons are handled as client area (WM_LBUTTONDOWN)
                int btnW = static_cast<int>(46 * dpix / 96.0f);
                if (pt.x > w - btnW * 3) return HTCLIENT; // button area
                return HTCAPTION; // draggable title bar
            }

            return HTCLIENT;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_NCACTIVATE:
            // Prevent default non-client area painting (white border on deactivate)
            return TRUE;
        case WM_NCPAINT:
            // Skip default non-client painting
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---- Signaling setup ----

void App::initSignaling() {
    signaling_ = std::make_unique<SignalingClient>();

    signaling_->onConnectionRequest([this](const ConnectionRequest& req) {
        UserID fromId = req.fromId;
        queueAction([this, fromId]() { handleConnectionRequest(fromId); });
    });
    signaling_->onConnectionRejected([this](const ConnectionRejection&) {
        queueAction([this]() {
            statusMessage_ = "Connection rejected by host";
            state_ = AppState::DASHBOARD;
            connectTimeoutSec_ = 0.0f;
        });
    });
    signaling_->onUserOffline([this](const UserID&) {
        queueAction([this]() {
            statusMessage_ = "User is offline or not found";
            state_ = AppState::DASHBOARD;
            connectTimeoutSec_ = 0.0f;
        });
    });
    signaling_->onDisconnected([this]() {
        queueAction([this]() {
            if (state_ == AppState::CONNECTING) {
                statusMessage_ = "Lost connection to relay server";
                state_ = AppState::DASHBOARD;
                connectTimeoutSec_ = 0.0f;
            }
        });
    });
    signaling_->onRegistered([this](bool success) {
        queueAction([this, success]() {
            if (!success) {
                statusMessage_ = "Failed to register with relay server";
            } else {
                LOG_INFO("Registered with relay server as %s", myId_.id.c_str());
                invalidate(); // update Online status
            }
        });
    });
    signaling_->onSdpOffer([this](const SdpMessage& msg) {
        queueAction([this, msg]() {
            if (webrtcSession_) webrtcSession_->onRemoteDescription(msg.sdp, "offer");
        });
    });
    signaling_->onSdpAnswer([this](const SdpMessage& msg) {
        queueAction([this, msg]() {
            if (webrtcSession_) webrtcSession_->onRemoteDescription(msg.sdp, "answer");
        });
    });
    signaling_->onIceCandidate([this](const IceCandidateMsg& ice) {
        queueAction([this, ice]() {
            if (webrtcSession_) webrtcSession_->onRemoteCandidate(ice.candidate, ice.sdpMid);
        });
    });
}

// ---- Connection handling ----

void App::handleConnectionRequest(const UserID& fromUser) {
    pendingConnectionFrom_ = fromUser;
    showConnectionDialog_ = true;
    invalidate();
    LOG_INFO("Connection request from %s", fromUser.id.c_str());
}

void App::connectToPeer(const std::string& peerId) {
    if (peerId.empty() || peerId.length() != 8) {
        statusMessage_ = "Invalid ID (must be 8 characters)";
        invalidate();
        return;
    }
    if (!signaling_ || !signaling_->isConnected()) {
        statusMessage_ = "Not connected to signaling server";
        invalidate();
        return;
    }
    if (!signaling_->isRegistered()) {
        statusMessage_ = "Not yet registered with server, please wait";
        invalidate();
        return;
    }

    std::string normalizedId = peerId;
    for (char& c : normalizedId) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    UserID targetId{normalizedId};

    signaling_->onConnectionAccepted([this, targetId](const ConnectionAcceptance& acc) {
        queueAction([this, targetId, acc]() {
            connectTimeoutSec_ = 0.0f;
            viewerSession_ = std::make_unique<ViewerSession>();
            if (!viewerSession_->start()) {
                statusMessage_ = "Failed to start viewer session";
                state_ = AppState::DASHBOARD;
                return;
            }
            if (webrtcSession_) {
                webrtcSession_->setOnDisconnected(nullptr);
                webrtcSession_->close();
                webrtcSession_.reset();
            }
            WebRtcConfig wrtcCfg;
            wrtcCfg.turnServer = config_.turnServer;
            wrtcCfg.turnUser = config_.turnUser;
            wrtcCfg.turnPassword = config_.turnPassword;
            webrtcSession_ = std::make_unique<WebRtcSession>(signaling_.get(), acc.fromId, wrtcCfg);
            webrtcSession_->setOnConnected([this]() {
                LOG_INFO("WebRTC connected! Switching to SESSION_VIEWER");
                queueAction([this]() {
                    state_ = AppState::SESSION_VIEWER;
                    inputHandler_ = std::make_unique<InputHandler>();
                    inputHandler_->init(glHwnd_, config_.encoder.width, config_.encoder.height);
                    inputHandler_->setMouseCallback([this](const MouseEvent& mev) {
                        if (!webrtcSession_) return;
                        InputEvent ev;
                        ev.x = mev.x; ev.y = mev.y;
                        if (mev.scrollX != 0 || mev.scrollY != 0) {
                            ev.type = InputType::MOUSE_SCROLL;
                            ev.y = static_cast<int32_t>(mev.scrollY);
                        } else if (mev.buttons != 0) {
                            ev.type = mev.pressed ? InputType::MOUSE_DOWN : InputType::MOUSE_UP;
                            if (mev.buttons & 1) ev.button = 0;
                            else if (mev.buttons & 2) ev.button = 1;
                            else if (mev.buttons & 4) ev.button = 2;
                        } else {
                            ev.type = InputType::MOUSE_MOVE;
                        }
                        uint8_t buf[1 + InputEvent::SIZE];
                        buf[0] = 0x01;
                        ev.serialize(buf + 1);
                        webrtcSession_->sendData(buf, sizeof(buf));
                    });
                    inputHandler_->setKeyCallback([this](const KeyEvent& kev) {
                        if (!webrtcSession_) return;
                        InputEvent ev;
                        ev.type = kev.pressed ? InputType::KEY_DOWN : InputType::KEY_UP;
                        ev.scancode = kev.scancode;
                        ev.pressed = kev.pressed;
                        uint8_t buf[1 + InputEvent::SIZE];
                        buf[0] = 0x01;
                        ev.serialize(buf + 1);
                        webrtcSession_->sendData(buf, sizeof(buf));
                    });
                    inputHandler_->setEnabled(true);
                });
            });
            webrtcSession_->setOnDisconnected([this]() {
                queueAction([this]() { disconnectSession(); });
            });
            webrtcSession_->setOnVideo([this](const uint8_t* data, size_t size) {
                static int vcount = 0;
                if (++vcount <= 5) LOG_INFO("Video packet #%d, size=%zu", vcount, size);
                if (viewerSession_) viewerSession_->onNalUnit(data, size);
            });
            webrtcSession_->setOnData([this](const uint8_t*, size_t) {});
            webrtcSession_->startAsViewer();
        });
    });

    if (!signaling_->requestConnection(targetId)) {
        statusMessage_ = "Failed to send connection request";
        invalidate();
        return;
    }

    connectTimeoutSec_ = 0.0f;
    connectIdInput_ = normalizedId;
    state_ = AppState::CONNECTING;
    invalidate();
}

void App::disconnectSession() {
    if (inputHandler_) {
        inputHandler_->setEnabled(false);
        inputHandler_.reset();
    }
    inputInjector_.reset();
    if (webrtcSession_) webrtcSession_->close();
    webrtcSession_.reset();
    hostSession_.reset();
    viewerSession_.reset();
    state_ = AppState::DASHBOARD;
    statusMessage_ = "Disconnected";
    invalidate();
    LOG_INFO("Session disconnected");
}

void App::tryConnectSignaling() {
    if (signaling_->isConnected()) return;
    if (!signaling_->connect(config_.signalingHost, config_.signalingPort,
                             config_.signalingFallbackPorts)) {
        LOG_WARN("Could not reach relay server %s on any port",
                 config_.signalingHost.c_str());
        queueAction([this]() {
            statusMessage_ = "Relay server unreachable — retrying...";
        });
    } else {
        LOG_INFO("Connected to relay %s:%d",
                 config_.signalingHost.c_str(), signaling_->connectedPort());
        signaling_->registerUser(myId_);
    }
}

void App::startReconnectThread() {
    reconnectThread_ = std::thread([this]() {
        for (int i = 0; i < 50 && appRunning_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (appRunning_) {
            if (!signaling_->isConnected()) {
                LOG_INFO("Reconnect thread: signaling offline, retrying...");
                tryConnectSignaling();
            }
            for (int i = 0; i < 50 && appRunning_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void App::shutdown() {
    appRunning_ = false;
    if (reconnectThread_.joinable()) reconnectThread_.join();

    if (inputHandler_) { inputHandler_->setEnabled(false); inputHandler_.reset(); }
    inputInjector_.reset();
    if (webrtcSession_) webrtcSession_->close();
    webrtcSession_.reset();
    hostSession_.reset();
    viewerSession_.reset();
    signaling_.reset();

    // D2D cleanup
    if (g_d2dBrush) { g_d2dBrush->Release(); g_d2dBrush = nullptr; }
    if (g_d2dRT) { g_d2dRT->Release(); g_d2dRT = nullptr; }
    if (g_fmtNormal) { g_fmtNormal->Release(); g_fmtNormal = nullptr; }
    if (g_fmtBold) { g_fmtBold->Release(); g_fmtBold = nullptr; }
    if (g_fmtSmall) { g_fmtSmall->Release(); g_fmtSmall = nullptr; }
    if (g_fmtLarge) { g_fmtLarge->Release(); g_fmtLarge = nullptr; }
    if (g_fmtXL) { g_fmtXL->Release(); g_fmtXL = nullptr; }
    if (g_dwFactory) { g_dwFactory->Release(); g_dwFactory = nullptr; }
    if (g_d2dFactory) { g_d2dFactory->Release(); g_d2dFactory = nullptr; }

    if (glRC_) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glRC_); glRC_ = nullptr; }
    if (glDC_) { ReleaseDC(glHwnd_, glDC_); glDC_ = nullptr; }
    if (glHwnd_) { DestroyWindow(glHwnd_); glHwnd_ = nullptr; }
    if (mainHwnd_) { DestroyWindow(mainHwnd_); mainHwnd_ = nullptr; }
    if (wakeEvent_) { CloseHandle(wakeEvent_); wakeEvent_ = nullptr; }
    CoUninitialize();
}

} // namespace omnidesk
