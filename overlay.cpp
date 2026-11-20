#include "overlay.hpp"
#include "logger.hpp"
#include "strings.hpp"
#include <dwmapi.h>
#include <cstdio>
#include <chrono>
#include <cwctype>

struct EnumData {
    DWORD targetPid;
    HWND bestHwnd;
    int bestArea;
};

static bool IsCs2Process(DWORD pid) {
    if (!pid) return false;

    /* memo: window enumeration asks about the same handful of pids over and
     * over, and each miss otherwise costs an OpenProcess round trip */
    static DWORD lastPid = 0;
    static bool lastResult = false;
    static auto lastAt = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (pid == lastPid && (now - lastAt) < std::chrono::milliseconds(3000)) return lastResult;

    bool ok = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            std::wstring p(path);
            const size_t pos = p.find_last_of(L'\\');
            const std::wstring base = (pos == std::wstring::npos) ? p : p.substr(pos + 1);
            ok = (_wcsicmp(base.c_str(), strs::Decode(strs::CS2EXE).c_str()) == 0);
        }
        CloseHandle(h);
    }

    lastPid = pid;
    lastResult = ok;
    lastAt = now;
    return ok;
}

static BOOL CALLBACK EnumWindowProc(HWND hwnd, LPARAM lParam) {
    EnumData* data = reinterpret_cast<EnumData*>(lParam);

    /* class names drift between game builds (SDL2 to SDL3), so the only
     * identity we trust is the process image. cheap geometry filters run
     * first so the image check only fires on plausible candidates. */
    if (!IsWindowVisible(hwnd)) return TRUE;

    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w < 200 || h < 200) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    if (data->targetPid != 0 && pid != data->targetPid) return TRUE;
    if (!IsCs2Process(pid)) return TRUE;

    const int area = w * h;

    if (area > data->bestArea) {
        data->bestArea = area;
        data->bestHwnd = hwnd;
        data->targetPid = pid;
    }

    return TRUE;
}

bool Overlay::Initialize() {
    wchar_t buf[32] = {};
    swprintf_s(buf, L"w%08x%04x", GetTickCount(), GetCurrentProcessId() & 0xFFFF);
    class_name = buf;

    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name.c_str();

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"",
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!hwnd) {
        return false;
    }

    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, width, height, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);

    initialized = true;
    return true;
}

void Overlay::Shutdown() {
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (wc.hInstance) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
    initialized = false;
}

void Overlay::UpdateSize(int w, int h) {
    width = w;
    height = h;
    if (hwnd) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, width, height, SWP_NOMOVE | SWP_NOACTIVATE);
    }
}

bool Overlay::IsGameForeground() const {
    if (!gameHwnd || !gamePid) return false;
    if (IsIconic(gameHwnd)) return false;
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == gamePid;
}

bool Overlay::SyncToGameWindow(bool* size_changed) {
    if (size_changed) *size_changed = false;

    static auto lastEnum = std::chrono::steady_clock::time_point{};
    static auto lastValidate = std::chrono::steady_clock::time_point{};
    static DWORD cachedPid = 0;
    static bool cachedPidIsCs2 = false;
    const auto now = std::chrono::steady_clock::now();

    /* the image-name check opens the target process. running it every frame
     * (and once per visible window on every enumerate) was one of the periodic
     * hitches in the overlay: hundreds of OpenProcess calls per second. the hwnd
     * and pid checks stay per-frame because they cost nothing. */
    auto validateCached = [&]() -> bool {
        if (!gameHwnd || !IsWindow(gameHwnd) || !IsWindowVisible(gameHwnd)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(gameHwnd, &pid);
        if (pid == 0) return false;
        if (gamePid != 0 && pid != gamePid) return false;
        if (pid == cachedPid && (now - lastValidate) < std::chrono::milliseconds(1000)) {
            return cachedPidIsCs2;
        }
        cachedPid = pid;
        cachedPidIsCs2 = IsCs2Process(pid);
        lastValidate = now;
        return cachedPidIsCs2;
    };

    bool needEnum = !validateCached();
    if (needEnum && (now - lastEnum) < std::chrono::milliseconds(500)) {
        /* while the game is absent, do not walk the desktop every frame */
        return false;
    }
    if (!needEnum && (now - lastEnum) > std::chrono::milliseconds(4000)) {
        // Periodic refresh to pick larger/better SDL window if created later
        needEnum = true;
    }

    if (needEnum) {
        EnumData data = { 0, nullptr, 0 };
        EnumWindows(EnumWindowProc, reinterpret_cast<LPARAM>(&data));
        if (data.bestHwnd) {
            gameHwnd = data.bestHwnd;
            gamePid = data.targetPid;
        } else {
            gameHwnd = nullptr;
            gamePid = 0;
        }
        lastEnum = now;
    }

    if (!gameHwnd) {
        static int notFoundCount = 0;
        if (++notFoundCount % 100 == 0) {
            Log("[!] game window not found yet");
        }
        return false;
    }

    /* park when the game is not in front. an always-on-top layer left showing
     * while CS2 is alt-tabbed or minimized keeps compositing a stale frame over
     * the desktop and keeps forcing z-order passes against a fullscreen
     * flip-model swapchain. that is the only GPU-facing work this process does,
     * so it is the only thing here that can plausibly disturb the driver. */
    static bool parked = false;
    if (!IsGameForeground()) {
        if (!parked) {
            ShowWindow(hwnd, SW_HIDE);
            parked = true;
        }
        return false;
    }
    if (parked) {
        ShowWindow(hwnd, SW_SHOWNA);
        parked = false;
    }

    RECT rc;
    if (!GetClientRect(gameHwnd, &rc)) {
        gameHwnd = nullptr;
        gamePid = 0;
        return false;
    }

    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w < 64 || h < 64) {
        return false;
    }

    POINT tl = { 0, 0 };
    ClientToScreen(gameHwnd, &tl);

    const bool resized = (w != width || h != height);
    if (resized) {
        width = w;
        height = h;
        if (size_changed) *size_changed = true;
    }

    /* SetWindowPos is synchronous and forces a z-order/composition pass. it was
     * being called every single frame; now only when the target rect actually
     * moves or resizes, plus a lazy topmost re-assert in case something else
     * stole the top of the z-order. */
    static int lastX = INT_MIN, lastY = INT_MIN, lastW = -1, lastH = -1;
    static auto lastTopmost = std::chrono::steady_clock::time_point{};
    const bool moved = (tl.x != lastX || tl.y != lastY || width != lastW || height != lastH);
    if (moved || (now - lastTopmost) > std::chrono::milliseconds(8000)) {
        SetWindowPos(hwnd, HWND_TOPMOST, tl.x, tl.y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        lastX = tl.x; lastY = tl.y; lastW = width; lastH = height;
        lastTopmost = now;
    }
    return true;
}

bool Overlay::ProcessMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
}
