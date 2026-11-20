#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include "overlay.hpp"

struct ImDrawData;

class Renderer {
private:
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* renderTargetView = nullptr;

    HWND hwnd = nullptr;
    bool initialized = false;

public:
    bool Initialize(HWND window);
    void Shutdown();
    void BeginFrame();
    void EndFrame();
    void Resize(int width, int height);
};
