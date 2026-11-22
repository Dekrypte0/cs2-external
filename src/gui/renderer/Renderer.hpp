#pragma once

#include <atomic>

class Renderer {
public:
    ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    static bool Init();
    static void Destroy();
    static void Thread();

    static bool IsOpen();
    static bool IsFocused();
private:
    Renderer() {};

    static Renderer& GetInstance()
    {
        static Renderer i{};
        return i;
    }

    bool InitImpl();
    void ThreadImpl();
    void DestroyImpl();

    void Render();
    void HandleOverlayHotkey();
    bool HandleState();
    bool HandleWindowOrder();
private:
    bool isRunning = true;
    bool isOpen = false;

    // F3 hides only the window. The engine, scripting and render loops keep
    // running at full speed while the overlay is hidden.
    bool overlay_hidden = false;
    bool overlay_visible = true;

    std::atomic<bool> isFocused{false};
};
