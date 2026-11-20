#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <cstdint>

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

class Overlay {
private:
    HWND hwnd = nullptr;
    HWND gameHwnd = nullptr;
    DWORD gamePid = 0;
    WNDCLASSEXW wc = {};
    int width = 1920;
    int height = 1080;
    bool initialized = false;

    // Randomized per launch to avoid a fixed well-known window class
    std::wstring class_name;

public:
    bool Initialize();
    void Shutdown();
    HWND GetHWND() const { return hwnd; }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

    void UpdateSize(int w, int h);

    // Track the CS2 window (class SDL_app): position + size follow it.
    // Returns false while it can't be found. size_changed flips once per resize.
    bool SyncToGameWindow(bool* size_changed);

    // True only when the tracked CS2 window owns the foreground. The overlay
    // hides itself otherwise; nothing is composited over other applications.
    bool IsGameForeground() const;

    bool ProcessMessages();
};
