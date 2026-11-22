#include "Renderer.hpp"
#include "window/Window.hpp"

#include "core/engine/Engine.hpp"
#include "gui/frontend/esp/Esp.hpp"
#include "gui/frontend/menu/Menu.hpp"
#include "gui\frontend\overlays\Overlays.hpp"
#include "core\scripting\Scripting.hpp"
#include "gui/frontend/theme/Theme.hpp"

bool Renderer::Init() {
    return GetInstance().InitImpl();
}

void Renderer::Thread() {
    return GetInstance().ThreadImpl();
}

void Renderer::Destroy() {
    return GetInstance().DestroyImpl();
}   

bool Renderer::IsOpen() {
    return GetInstance().isOpen;
}

bool Renderer::IsFocused() {
    return GetInstance().isFocused.load();
}

bool Renderer::InitImpl() {
    if (!Window::SpawnWindow()) {
        LOGF(FATAL, "Failed to create window");
        return false;
    }

    if (!Window::CreateDevice()) {
        LOGF(FATAL, "Failed to create device");
        return false;
    }

    if (!Window::CreateImGui()) {
        LOGF(FATAL, "Failed to create ImGui");
        return false;
    }

    Menu::Init();
    Esp::Init();
    Overlays::Init();
    scripting::Scripting::Get().Init();

    // Focus the game
    SetForegroundWindow(Engine::GetProcess()->hwnd_);

    if (cfg::settings::streamproof)
        Window::SetAffinity(Window::hwnd, WindowAffinity::Invisible);

    if (cfg::settings::vsync)
        Window::vsync = true;

    // We want the main thread to call render
    // And lock it
    // std::thread(Thread).detach();

    LOGF(INFO, "Successfully initialized renderer...");
    return true;
}

void Renderer::DestroyImpl() {
    isRunning = false; // Prepare to stop thread loop
    LOGF(VERBOSE, "Successfully programed renderer destruction...");
}

void Renderer::ThreadImpl() {
    while (isRunning) {
        const auto frame_start = std::chrono::steady_clock::now();

        // Cap the overlay so it cannot out-run the game or peg a GPU core.
        // 4167 us is ~240 Hz: smooth, but far cheaper than an unthrottled Present.
        if (cfg::settings::free_cpu)
            std::this_thread::sleep_until(frame_start + std::chrono::microseconds(4167));

        // Hidden by F3: keep the loop and hotkey alive, skip all draw work.
        if (!overlay_hidden)
            Render();

        // F3 is global so it still works while the overlay is hidden.
        HandleOverlayHotkey();

        // If the game is not focused dont do states, 
        // or will start focusing game & overlay
        if (this->isFocused && HandleState())
            continue; // It will cause flickering if we handle window order after window closes

        HandleWindowOrder();
    }

    // Once exited, destroy everything
    Window::DestroyImGui();
    Window::DestroyDevice();
    Window::DespawnWindow();
}

void Renderer::Render() {
    theme::ThemeManager::Get().FlushPending();

    Window::StartRender();

    Esp::Render();
    Overlays::Render();

    Menu::RenderStartupHelp();
    if (isOpen) Menu::Render();

    Window::EndRender();
}

void Renderer::HandleOverlayHotkey() {
    static bool was_f3 = false;
    const bool pressed = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;

    // F3 hides the window only. Reads, scripts and the render loop stay live.
    if (!was_f3 && pressed) {
        overlay_hidden = !overlay_hidden;
        if (overlay_hidden) {
            ShowWindow(Window::hwnd, SW_HIDE);
            overlay_visible = false;
        } else {
            ShowWindow(Window::hwnd, SW_SHOW);
            overlay_visible = true;
            Window::SetClickthrough(Window::hwnd, !isOpen);
        }
        LOGF(VERBOSE, "F3 toggled overlay hidden to {}", overlay_hidden);
    }
    was_f3 = pressed;
}

bool Renderer::HandleState() {
    isRunning = Window::shouldRun; // From the window event handler
    scripting::Scripting::Get().Update();

    static bool was_holding = false;

    bool pressed_insert = (GetAsyncKeyState(VK_INSERT) & 0x8000);
    bool pressed_rshift = (GetAsyncKeyState(VK_RSHIFT) & 0x8000);

    bool pressed_end = (GetAsyncKeyState(VK_END) & 0x8000);

    bool should_toggle = !was_holding && (pressed_insert || pressed_rshift);

    if (should_toggle || pressed_end) { // Toggle when pressing end to trigger the config save :v
        this->isOpen = !isOpen;

        // Release cursor when opening the menu
        // Sometimes flashes the render as its handling the window order
        if (this->isOpen)
            SetForegroundWindow(Window::hwnd);
        else
            SetForegroundWindow(Engine::GetProcess()->hwnd_);

        Window::SetClickthrough(Window::hwnd, !this->isOpen);
        LOGF(VERBOSE, "Captured global VK_INSERT or VK_RSHIFT, toggling menu state to {}", this->isOpen);

        // Not the best way, but wont bother the user
        // As far as i know, no one has complained about the config saving system :D
        std::thread(Config::Write).detach(); // Not needed, but just in case
    }

    if (pressed_end)
        this->isRunning = false;

    was_holding = pressed_insert || pressed_rshift;
    return should_toggle;
}

bool Renderer::HandleWindowOrder() {
    auto p = Engine::GetProcess();

    if (!p || (!p->hwnd_ && !p->UpdateHWND()))
        return false;

    // Check if game window is still valid, if not, most likely game closed
    if (!IsWindow(p->hwnd_))
        this->isRunning = false;

    // Hidden by F: stay hidden regardless of focus changes, but keep the
    // render and update loops above running.
    if (overlay_hidden) {
        if (overlay_visible) {
            ShowWindow(Window::hwnd, SW_HIDE);
            overlay_visible = false;
        }
        return true;
    }

    auto foreground = GetForegroundWindow();
    this->isFocused = (foreground == Window::hwnd || foreground == p->hwnd_);

    if (!this->isFocused && overlay_visible) {
        LOGF(VERBOSE, "Hiding overlay window as the game its not focused");
        ShowWindow(Window::hwnd, SW_HIDE);
        overlay_visible = false;
        return true;
    }

    if (!overlay_visible && this->isFocused) {  
        LOGF(VERBOSE, "Showing overlay window as the game is focused");
        ShowWindow(Window::hwnd, SW_SHOW);
        overlay_visible = true;
        return true;
    }

    static RECT last_rect = { 0, 0, 0, 0 };

    RECT window_rect;
    if (!GetWindowRect(p->hwnd_, &window_rect))
        return false;

    // All good, no movements from the client
    if (memcmp(&window_rect, &last_rect, sizeof(RECT)) == 0)
        return true;

    RECT client_rect;
    if (!GetClientRect(p->hwnd_, &client_rect))
        return false;

    POINT top_left = { client_rect.left, client_rect.top };
    POINT bottom_right = { client_rect.right, client_rect.bottom };

    ClientToScreen(p->hwnd_, &top_left);
    ClientToScreen(p->hwnd_, &bottom_right);

    RECT screen_rect = { top_left.x, top_left.y, bottom_right.x, bottom_right.y };

    SetWindowPos(
        Window::hwnd,
        HWND_TOPMOST,
        screen_rect.left,
        screen_rect.top,
        screen_rect.right - screen_rect.left,
        screen_rect.bottom - screen_rect.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );

    last_rect = window_rect;

    return true;
}
