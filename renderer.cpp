#include "renderer.hpp"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "logger.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool Renderer::Initialize(HWND window) {
    hwnd = window;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &sd, &swapChain, &device, &featureLevel, &context
    );

    if (FAILED(hr)) {
        return false;
    }

    ID3D11Texture2D* backBuffer;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    device->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView);
    backBuffer->Release();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* Unicode glyph coverage: the built-in ImGui font is ASCII-only, so
     * Cyrillic names (КИБЕР_ПАПА, крымский Барс) and symbol tags wouldn't
     * render. Segoe UI carries all of it; fall back to the built-in font if
     * the TTF is somehow unavailable. */
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 2;
    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF, // basic latin + latin supplement
        0x0100, 0x017F, // latin extended-A (turkish/polish/czech diacritics)
        0x0370, 0x03FF, // greek
        0x0400, 0x04FF, // cyrillic + cyrillic supplement
        0x1E00, 0x1EFF, // latin extended additional (vietnamese)
        0x2000, 0x206F, // general punctuation
        0x2190, 0x21FF, // arrows
        0x2600, 0x27BF, // misc symbols + dingbats (♥ etc.)
        0
    };
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f, &fontCfg, glyphRanges)) {
        io.Fonts->AddFontDefault();
    }

    /* CJK coverage. Segoe UI has no Chinese/Japanese/Korean glyphs, and on
     * casual servers a large share of names are exactly that — a missing
     * glyph blanks the whole line. Merge the Windows CJK faces into the same
     * font atlas; every face is optional and skipped when absent. */
    auto mergeFont = [&](const char* path, float size, const ImWchar* ranges, int faceIndex) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontNo = faceIndex;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(path, size, &cfg, ranges);
    };
    mergeFont("C:\\Windows\\Fonts\\msyh.ttc", 14.0f, io.Fonts->GetGlyphRangesChineseSimplifiedCommon(), 0);
    mergeFont("C:\\Windows\\Fonts\\malgun.ttf", 14.0f, io.Fonts->GetGlyphRangesKorean(), 0);

    /* build the atlas eagerly and say what landed in it: one boot line beats
     * discovering missing glyphs mid-match. */
    io.Fonts->Build();
    Log("[+] font atlas: " + std::to_string(io.Fonts->TexWidth) + "x" + std::to_string(io.Fonts->TexHeight) +
        ", " + std::to_string(io.Fonts->Fonts.empty() ? 0 : io.Fonts->Fonts[0]->Glyphs.Size) + " glyphs");

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.Alpha = 1.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.08f, 0.09f, 0.11f, 0.96f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.10f, 0.11f, 0.14f, 0.90f);
    c[ImGuiCol_Border]           = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.24f, 0.30f, 0.38f, 1.00f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.28f, 0.36f, 0.46f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.28f, 0.36f, 0.46f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.32f, 0.42f, 0.54f, 1.00f);
    c[ImGuiCol_CheckMark]        = ImVec4(0.45f, 0.78f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.45f, 0.78f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.85f, 1.00f, 1.00f);
    c[ImGuiCol_Separator]        = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_Tab]              = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.28f, 0.36f, 0.46f, 1.00f);
    c[ImGuiCol_TabActive]        = ImVec4(0.22f, 0.28f, 0.36f, 1.00f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    initialized = true;
    return true;
}

void Renderer::Shutdown() {
    if (!initialized) return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (renderTargetView) { renderTargetView->Release(); renderTargetView = nullptr; }
    if (swapChain) { swapChain->Release(); swapChain = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }

    initialized = false;
}

void Renderer::BeginFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Renderer::EndFrame() {
    ImGui::Render();

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetRenderTargets(1, &renderTargetView, nullptr);
    context->ClearRenderTargetView(renderTargetView, clearColor);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    swapChain->Present(1, 0);
}

void Renderer::Resize(int width, int height) {
    if (!initialized) return;

    if (renderTargetView) {
        renderTargetView->Release();
        renderTargetView = nullptr;
    }

    DXGI_SWAP_CHAIN_DESC sd;
    swapChain->GetDesc(&sd);
    swapChain->ResizeBuffers(sd.BufferCount, width, height, sd.BufferDesc.Format, sd.Flags);

    ID3D11Texture2D* backBuffer;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    device->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView);
    backBuffer->Release();
}
