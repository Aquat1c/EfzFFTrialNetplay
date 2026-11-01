#include "render_dx9_imgui.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
constexpr DWORD kFrameQuadFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

struct FrameQuadVertex
{
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
};

struct PresenterState
{
    IDirect3D9 *d3d = nullptr;
    IDirect3DDevice9 *device = nullptr;
    IDirect3DTexture9 *frameTexture = nullptr;
    IDirect3DTexture9 *scaleTexture = nullptr;
    int scaleTextureW = 0;
    int scaleTextureH = 0;
    D3DPRESENT_PARAMETERS presentParams = {};
    HWND window = nullptr;
    bool imguiInitialized = false;
    bool menuVisible = false;
    bool showDemoWindow = false;
    bool deviceLost = false;
    bool appActive = true;
    int textureWidth = 0;
    int textureHeight = 0;
    EfzDx9NetplayHudProvider hudProvider = nullptr;
    void *hudProviderUserData = nullptr;
    EfzDx9OverlayProvider overlayProvider = nullptr;
    void *overlayProviderUserData = nullptr;
    EfzDx9TrainingMenuRendererFn trainingMenuRenderer = nullptr;
    EfzDx9TrainingMenuVisibleFn trainingMenuVisible = nullptr;
    EfzDx9TrainingMenuHandleMessageFn trainingMenuHandleMessage = nullptr;
    EfzDx9TrainingMenuWantsDrawFn trainingMenuWantsDraw = nullptr;
    EfzDx9TrainingMenuDeviceNotifyFn trainingMenuDeviceNotify = nullptr;
    void *trainingMenuUserData = nullptr;
    ImGuiContext *imguiContext = nullptr;
};

PresenterState g_presenter = {};

bool TrainingMenuShouldDraw()
{
    return g_presenter.trainingMenuRenderer != nullptr
        && (g_presenter.trainingMenuVisible == nullptr
            || g_presenter.trainingMenuVisible(g_presenter.trainingMenuUserData));
}

bool TrainingMenuWantsDraw()
{
    if (g_presenter.trainingMenuRenderer == nullptr)
        return false;
    if (g_presenter.trainingMenuWantsDraw != nullptr)
        return g_presenter.trainingMenuWantsDraw(g_presenter.trainingMenuUserData) != FALSE;
    return TrainingMenuShouldDraw();
}

bool PresenterNeedsImGuiPath()
{
    return g_presenter.menuVisible
        || g_presenter.hudProvider != nullptr
        || g_presenter.overlayProvider != nullptr;
}

bool PresenterNeedsAnyOverlay()
{
    return PresenterNeedsImGuiPath() || TrainingMenuWantsDraw();
}

ImDrawList* WrapperOverlayDrawList()
{
    return g_presenter.menuVisible ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
}

void RenderTrainingMenu(const EfzDx9PresentParams *params)
{
    if (params == nullptr || !TrainingMenuWantsDraw())
    {
        return;
    }

    ImGuiContext *savedContext = ImGui::GetCurrentContext();
    g_presenter.trainingMenuRenderer(
        g_presenter.device,
        g_presenter.window,
        params,
        g_presenter.trainingMenuUserData);
    if (savedContext != nullptr)
    {
        ImGui::SetCurrentContext(savedContext);
    }
}

void ReleaseFrameTexture()
{
    if (g_presenter.frameTexture != nullptr)
    {
        g_presenter.frameTexture->Release();
        g_presenter.frameTexture = nullptr;
    }
    g_presenter.textureWidth = 0;
    g_presenter.textureHeight = 0;
}

// D3DPOOL_DEFAULT texture: must be released before any device->Reset().
void ReleaseScaleTexture()
{
    if (g_presenter.scaleTexture != nullptr)
    {
        g_presenter.scaleTexture->Release();
        g_presenter.scaleTexture = nullptr;
    }
    g_presenter.scaleTextureW = 0;
    g_presenter.scaleTextureH = 0;
}

void ShutdownImGui()
{
    if (!g_presenter.imguiInitialized)
    {
        return;
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_presenter.imguiContext = nullptr;
    g_presenter.imguiInitialized = false;
}

void DestroyDevice()
{
    ReleaseFrameTexture();
    ReleaseScaleTexture();
    ShutdownImGui();

    if (g_presenter.device != nullptr)
    {
        g_presenter.device->Release();
        g_presenter.device = nullptr;
    }
    if (g_presenter.d3d != nullptr)
    {
        g_presenter.d3d->Release();
        g_presenter.d3d = nullptr;
    }

    g_presenter.window = nullptr;
    g_presenter.deviceLost = false;
}

bool InitializeImGui(HWND window)
{
    if (g_presenter.imguiInitialized)
    {
        return true;
    }
    if (g_presenter.device == nullptr || window == nullptr)
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    g_presenter.imguiContext = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(window))
    {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX9_Init(g_presenter.device))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    g_presenter.imguiInitialized = true;
    return true;
}

bool CreateDevice(HWND window, int clientWidth, int clientHeight)
{
    DestroyDevice();

    g_presenter.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (g_presenter.d3d == nullptr)
    {
        return false;
    }

    ZeroMemory(&g_presenter.presentParams, sizeof(g_presenter.presentParams));
    g_presenter.presentParams.Windowed = TRUE;
    g_presenter.presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_presenter.presentParams.BackBufferFormat = D3DFMT_UNKNOWN;
    g_presenter.presentParams.EnableAutoDepthStencil = FALSE;
    g_presenter.presentParams.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    g_presenter.presentParams.hDeviceWindow = window;
    g_presenter.presentParams.BackBufferWidth = clientWidth > 0 ? static_cast<UINT>(clientWidth) : 0u;
    g_presenter.presentParams.BackBufferHeight = clientHeight > 0 ? static_cast<UINT>(clientHeight) : 0u;

    const DWORD behaviorFlags = D3DCREATE_FPU_PRESERVE | D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
    const HRESULT createHr = g_presenter.d3d->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        window,
        behaviorFlags,
        &g_presenter.presentParams,
        &g_presenter.device);
    if (FAILED(createHr))
    {
        DestroyDevice();
        return false;
    }

    g_presenter.window = window;
    return InitializeImGui(window);
}

bool ResetDevice(int clientWidth, int clientHeight)
{
    if (g_presenter.device == nullptr)
    {
        return false;
    }

    // Release ALL D3DPOOL_DEFAULT objects before Reset, or it fails with D3DERR_INVALIDCALL.
    if (g_presenter.imguiInitialized && g_presenter.imguiContext != nullptr)
    {
        ImGuiContext *saved = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_presenter.imguiContext);
        ImGui_ImplDX9_InvalidateDeviceObjects();
        ImGui::SetCurrentContext(saved);
    }
    if (g_presenter.trainingMenuDeviceNotify != nullptr)
    {
        g_presenter.trainingMenuDeviceNotify(0, g_presenter.trainingMenuUserData);
    }
    ReleaseFrameTexture();
    ReleaseScaleTexture();

    g_presenter.presentParams.BackBufferWidth = clientWidth > 0 ? static_cast<UINT>(clientWidth) : 0u;
    g_presenter.presentParams.BackBufferHeight = clientHeight > 0 ? static_cast<UINT>(clientHeight) : 0u;
    const HRESULT resetHr = g_presenter.device->Reset(&g_presenter.presentParams);
    if (FAILED(resetHr))
    {
        g_presenter.deviceLost = true;
        return false;
    }

    if (g_presenter.imguiInitialized && g_presenter.imguiContext != nullptr)
    {
        ImGuiContext *saved = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_presenter.imguiContext);
        ImGui_ImplDX9_CreateDeviceObjects();
        ImGui::SetCurrentContext(saved);
    }
    if (g_presenter.trainingMenuDeviceNotify != nullptr)
    {
        g_presenter.trainingMenuDeviceNotify(1, g_presenter.trainingMenuUserData);
    }

    g_presenter.deviceLost = false;
    return true;
}

bool EnsureDevice(const EfzDx9PresentParams &params)
{
    if (g_presenter.device == nullptr || g_presenter.window != params.window)
    {
        return CreateDevice(params.window, params.clientWidth, params.clientHeight);
    }

    if (g_presenter.deviceLost)
    {
        const HRESULT status = g_presenter.device->TestCooperativeLevel();
        if (status == D3DERR_DEVICELOST)
        {
            return false;
        }
        if (status == D3DERR_DEVICENOTRESET)
        {
            return ResetDevice(params.clientWidth, params.clientHeight);
        }
        g_presenter.deviceLost = false;
    }

    if (params.clientWidth > 0 && params.clientHeight > 0
        && (static_cast<UINT>(params.clientWidth) != g_presenter.presentParams.BackBufferWidth
            || static_cast<UINT>(params.clientHeight) != g_presenter.presentParams.BackBufferHeight))
    {
        return ResetDevice(params.clientWidth, params.clientHeight);
    }

    return true;
}

bool EnsureFrameTexture(int width, int height)
{
    if (g_presenter.device == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    if (g_presenter.frameTexture != nullptr && g_presenter.textureWidth == width && g_presenter.textureHeight == height)
    {
        return true;
    }

    ReleaseFrameTexture();
    const HRESULT createHr = g_presenter.device->CreateTexture(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &g_presenter.frameTexture,
        nullptr);
    if (FAILED(createHr))
    {
        return false;
    }

    g_presenter.textureWidth = width;
    g_presenter.textureHeight = height;
    return true;
}

bool EnsureScaleTexture(int width, int height)
{
    if (g_presenter.device == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }
    if (g_presenter.scaleTexture != nullptr && g_presenter.scaleTextureW == width && g_presenter.scaleTextureH == height)
    {
        return true;
    }
    ReleaseScaleTexture();
    const HRESULT createHr = g_presenter.device->CreateTexture(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &g_presenter.scaleTexture,
        nullptr);
    if (FAILED(createHr))
    {
        g_presenter.scaleTexture = nullptr;
        return false;
    }
    g_presenter.scaleTextureW = width;
    g_presenter.scaleTextureH = height;
    return true;
}

bool UploadIndexedFrame(const EfzDx9PresentParams &params)
{
    if (g_presenter.frameTexture == nullptr || params.pixels == nullptr)
    {
        return false;
    }

    D3DLOCKED_RECT lockedRect = {};
    if (FAILED(g_presenter.frameTexture->LockRect(0, &lockedRect, nullptr, 0)))
    {
        return false;
    }

    for (int y = 0; y < params.height; ++y)
    {
        const BYTE *srcRow = params.pixels + static_cast<size_t>(y) * static_cast<size_t>(params.pitch);
        auto *dstRow = reinterpret_cast<std::uint32_t *>(static_cast<BYTE *>(lockedRect.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lockedRect.Pitch));
        for (int x = 0; x < params.width; ++x)
        {
            const BYTE index = srcRow[x];
            BYTE red = index;
            BYTE green = index;
            BYTE blue = index;
            if (params.palette != nullptr)
            {
                const RGBQUAD &color = params.palette[index];
                red = color.rgbRed;
                green = color.rgbGreen;
                blue = color.rgbBlue;
            }
            dstRow[x] = 0xFF000000u
                | (static_cast<std::uint32_t>(red) << 16u)
                | (static_cast<std::uint32_t>(green) << 8u)
                | static_cast<std::uint32_t>(blue);
        }
    }

    g_presenter.frameTexture->UnlockRect(0);
    return true;
}

float ClampFloat(float value, float lo, float hi)
{
    return (std::max)(lo, (std::min)(hi, value));
}

float GameToScreenX(const EfzDx9PresentParams &params, float virtualX)
{
    return static_cast<float>(params.dstX)
        + virtualX * static_cast<float>(params.dstW) / 640.0f;
}

float GameToScreenY(const EfzDx9PresentParams &params, float virtualY)
{
    return static_cast<float>(params.dstY)
        + virtualY * static_cast<float>(params.dstH) / 480.0f;
}

float GameScale(const EfzDx9PresentParams &params)
{
    if (params.dstW <= 0 || params.dstH <= 0)
    {
        return 1.0f;
    }
    return ClampFloat(
        (std::min)(static_cast<float>(params.dstW) / 640.0f, static_cast<float>(params.dstH) / 480.0f),
        0.75f,
        3.0f);
}

ImVec2 MeasureText(ImFont *font, float fontSize, const char *text)
{
    if (font == nullptr || text == nullptr || text[0] == '\0')
    {
        return ImVec2(0.0f, 0.0f);
    }
    return font->CalcTextSizeA(fontSize, 1000000.0f, 0.0f, text);
}

std::string ClipTextToWidth(ImFont *font, float fontSize, const char *text, float maxWidth)
{
    if (font == nullptr || text == nullptr || text[0] == '\0' || maxWidth <= 0.0f)
    {
        return {};
    }

    std::string clipped(text);
    while (!clipped.empty() && MeasureText(font, fontSize, clipped.c_str()).x > maxWidth)
    {
        clipped.pop_back();
    }

    if (clipped.size() < std::strlen(text) && maxWidth > fontSize * 2.5f)
    {
        while (!clipped.empty() && MeasureText(font, fontSize, (clipped + "...").c_str()).x > maxWidth)
        {
            clipped.pop_back();
        }
        clipped += "...";
    }
    return clipped;
}

void DrawShadowedText(ImDrawList *drawList, ImFont *font, float fontSize, ImVec2 pos, ImU32 color, const char *text)
{
    if (drawList == nullptr || font == nullptr || text == nullptr || text[0] == '\0')
    {
        return;
    }

    drawList->AddText(font, fontSize, ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 180), text);
    drawList->AddText(font, fontSize, pos, color, text);
}

float DrawCompactHudLine(
    ImDrawList *drawList,
    ImFont *font,
    float fontSize,
    float scale,
    float anchorX,
    float topY,
    float maxWidth,
    bool rightAlign,
    const char *text,
    ImU32 textColor)
{
    if (drawList == nullptr || font == nullptr || text == nullptr || text[0] == '\0')
    {
        return topY;
    }

    const float padX = 5.0f * scale;
    const float padY = 3.0f * scale;
    const float rounding = 3.0f * scale;
    const std::string clipped = ClipTextToWidth(font, fontSize, text, (std::max)(1.0f, maxWidth - padX * 2.0f));
    if (clipped.empty())
    {
        return topY;
    }

    const ImVec2 textSize = MeasureText(font, fontSize, clipped.c_str());
    const float boxWidth = (std::min)(maxWidth, textSize.x + padX * 2.0f);
    const float boxHeight = fontSize + padY * 2.0f;
    const float left = rightAlign ? (anchorX - boxWidth) : anchorX;
    const float right = left + boxWidth;
    const float bottom = topY + boxHeight;

    drawList->AddRectFilled(ImVec2(left, topY), ImVec2(right, bottom), IM_COL32(0, 0, 0, 150), rounding);
    drawList->AddRect(ImVec2(left, topY), ImVec2(right, bottom), IM_COL32(72, 72, 72, 180), rounding);

    float textX = left + padX;
    if (rightAlign)
    {
        textX = right - padX - textSize.x;
    }
    DrawShadowedText(drawList, font, fontSize, ImVec2(textX, topY + padY), textColor, clipped.c_str());
    return bottom;
}

void DrawNetplayHud(const EfzDx9PresentParams &params)
{
    if (g_presenter.hudProvider == nullptr)
    {
        return;
    }

    EfzDx9NetplayHudState hud = {};
    hud.structSize = sizeof(hud);
    if (!g_presenter.hudProvider(&hud, g_presenter.hudProviderUserData) || !hud.visible)
    {
        return;
    }

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    const float scale = GameScale(params);
    const float fontSize = ImGui::GetFontSize() * scale;
    const float lineGap = 2.0f * scale;
    const float groupGap = 6.0f * scale;
    const float statusMaxWidth = 186.0f * scale;
    const float toastMaxWidth = 220.0f * scale;
    const float leftAnchor = GameToScreenX(params, 16.0f);
    const float rightAnchor = GameToScreenX(params, 624.0f);
    const float topY = GameToScreenY(params, 136.0f);

    float rightY = topY;
    if (hud.matchHeader[0] != '\0')
    {
        rightY = DrawCompactHudLine(
            drawList, font, fontSize, scale, rightAnchor, rightY, statusMaxWidth,
            true, hud.matchHeader, IM_COL32(205, 232, 255, 245)) + lineGap;
    }
    if (hud.sessionDetail[0] != '\0')
    {
        rightY = DrawCompactHudLine(
            drawList, font, fontSize, scale, rightAnchor, rightY, statusMaxWidth,
            true, hud.sessionDetail, IM_COL32(205, 232, 255, 245)) + lineGap;
    }

    const int debugCount = (std::max)(0, (std::min)(hud.debugLineCount, EFZ_DX9_NETPLAY_HUD_MAX_LINES));
    for (int i = 0; i < debugCount; ++i)
    {
        rightY = DrawCompactHudLine(
            drawList, font, fontSize, scale, rightAnchor, rightY, statusMaxWidth,
            true, hud.debugLines[i], IM_COL32(244, 244, 244, 245)) + lineGap;
    }

    float leftY = topY;
    const int practiceCount = (std::max)(0, (std::min)(hud.practiceLineCount, EFZ_DX9_NETPLAY_HUD_MAX_LINES));
    for (int i = 0; i < practiceCount; ++i)
    {
        leftY = DrawCompactHudLine(
            drawList, font, fontSize, scale, leftAnchor, leftY, statusMaxWidth,
            false, hud.practiceLines[i], IM_COL32(230, 230, 230, 245)) + lineGap;
    }

    const int toastCount = (std::max)(0, (std::min)(hud.toastLineCount, EFZ_DX9_NETPLAY_HUD_MAX_LINES));
    if (toastCount > 0)
    {
        const float lineHeight = fontSize + 6.0f * scale;
        const float toastBlockHeight = static_cast<float>(toastCount) * lineHeight
            + static_cast<float>((std::max)(0, toastCount - 1)) * lineGap;
        float toastY = (std::max)(
            leftY + groupGap,
            GameToScreenY(params, 480.0f - 54.0f) - toastBlockHeight);
        for (int i = 0; i < toastCount; ++i)
        {
            toastY = DrawCompactHudLine(
                drawList, font, fontSize, scale, leftAnchor, toastY, toastMaxWidth,
                false, hud.toastLines[i], IM_COL32(236, 236, 236, 245)) + lineGap;
        }
    }
}

ImU32 ArgbToImColor(DWORD color)
{
    const int a = static_cast<int>((color >> 24u) & 0xFFu);
    const int r = static_cast<int>((color >> 16u) & 0xFFu);
    const int g = static_cast<int>((color >> 8u) & 0xFFu);
    const int b = static_cast<int>(color & 0xFFu);
    return IM_COL32(r, g, b, a);
}

float OverlayScale(const EfzDx9PresentParams &params, const EfzDx9OverlayFrame &frame)
{
    const float virtualWidth = frame.virtualWidth > 0.0f ? frame.virtualWidth : 640.0f;
    const float virtualHeight = frame.virtualHeight > 0.0f ? frame.virtualHeight : 480.0f;
    if (params.dstW <= 0 || params.dstH <= 0)
    {
        return 1.0f;
    }
    return ClampFloat(
        (std::min)(static_cast<float>(params.dstW) / virtualWidth, static_cast<float>(params.dstH) / virtualHeight),
        0.5f,
        6.0f);
}

ImVec2 OverlayToScreen(const EfzDx9PresentParams &params, const EfzDx9OverlayFrame &frame, float x, float y)
{
    const float virtualWidth = frame.virtualWidth > 0.0f ? frame.virtualWidth : 640.0f;
    const float virtualHeight = frame.virtualHeight > 0.0f ? frame.virtualHeight : 480.0f;
    return ImVec2(
        static_cast<float>(params.dstX) + x * static_cast<float>(params.dstW) / virtualWidth,
        static_cast<float>(params.dstY) + y * static_cast<float>(params.dstH) / virtualHeight);
}

void DrawOverlayCommandText(
    ImDrawList *drawList,
    ImFont *font,
    const EfzDx9PresentParams &params,
    const EfzDx9OverlayFrame &frame,
    const EfzDx9OverlayCommand &command,
    float scale)
{
    if (command.text[0] == '\0')
    {
        return;
    }

    const float fontSize = (command.fontSize > 0.0f ? command.fontSize : ImGui::GetFontSize()) * scale;
    const float maxWidth = command.w > 0.0f ? command.w * scale : 0.0f;
    const std::string clipped = maxWidth > 0.0f
        ? ClipTextToWidth(font, fontSize, command.text, maxWidth)
        : std::string(command.text);
    if (clipped.empty())
    {
        return;
    }

    ImVec2 pos = OverlayToScreen(params, frame, command.x, command.y);
    const ImVec2 textSize = MeasureText(font, fontSize, clipped.c_str());
    if ((command.flags & EFZ_DX9_OVERLAY_TEXT_ALIGN_CENTER) != 0u)
    {
        pos.x -= textSize.x * 0.5f;
    }
    else if ((command.flags & EFZ_DX9_OVERLAY_TEXT_ALIGN_RIGHT) != 0u)
    {
        pos.x -= textSize.x;
    }

    if ((command.flags & EFZ_DX9_OVERLAY_TEXT_SHADOW) != 0u)
    {
        DrawShadowedText(drawList, font, fontSize, pos, ArgbToImColor(command.color), clipped.c_str());
        return;
    }

    drawList->AddText(font, fontSize, pos, ArgbToImColor(command.color), clipped.c_str());
}

void DrawGenericOverlay(const EfzDx9PresentParams &params)
{
    if (g_presenter.overlayProvider == nullptr)
    {
        return;
    }

    EfzDx9OverlayFrame frame = {};
    frame.structSize = sizeof(frame);
    if (!g_presenter.overlayProvider(&frame, g_presenter.overlayProviderUserData) || !frame.visible)
    {
        return;
    }

    ImDrawList *drawList = WrapperOverlayDrawList();
    ImFont *font = ImGui::GetFont();
    if (drawList == nullptr || font == nullptr)
    {
        return;
    }

    const int commandCount = (std::max)(0, (std::min)(frame.commandCount, EFZ_DX9_OVERLAY_MAX_COMMANDS));
    const float scale = OverlayScale(params, frame);
    const ImVec2 clipMin(static_cast<float>(params.dstX), static_cast<float>(params.dstY));
    const ImVec2 clipMax(static_cast<float>(params.dstX + params.dstW), static_cast<float>(params.dstY + params.dstH));
    drawList->PushClipRect(clipMin, clipMax, true);

    for (int i = 0; i < commandCount; ++i)
    {
        const EfzDx9OverlayCommand &command = frame.commands[i];
        if (command.structSize < sizeof(EfzDx9OverlayCommand))
        {
            continue;
        }

        const ImVec2 p0 = OverlayToScreen(params, frame, command.x, command.y);
        const ImVec2 p1 = OverlayToScreen(params, frame, command.x + command.w, command.y + command.h);
        switch (command.type)
        {
        case EFZ_DX9_OVERLAY_COMMAND_TEXT:
            DrawOverlayCommandText(drawList, font, params, frame, command, scale);
            break;
        case EFZ_DX9_OVERLAY_COMMAND_RECT_FILLED:
            drawList->AddRectFilled(p0, p1, ArgbToImColor(command.color));
            break;
        case EFZ_DX9_OVERLAY_COMMAND_RECT:
            drawList->AddRect(p0, p1, ArgbToImColor(command.color), 0.0f, 0, (std::max)(1.0f, command.thickness * scale));
            break;
        default:
            break;
        }
    }

    drawList->PopClipRect();
}

// Half-texel offset for D3D9's pixel-centre convention.
void DrawTexturedQuad(IDirect3DTexture9 *tex, float dx, float dy, float dw, float dh, DWORD filter)
{
    const float left = dx - 0.5f;
    const float top = dy - 0.5f;
    const float right = dx + dw - 0.5f;
    const float bottom = dy + dh - 0.5f;

    const FrameQuadVertex vertices[4] = {
        {left, top, 0.0f, 1.0f, 0.0f, 0.0f},
        {right, top, 0.0f, 1.0f, 1.0f, 0.0f},
        {left, bottom, 0.0f, 1.0f, 0.0f, 1.0f},
        {right, bottom, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    g_presenter.device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_presenter.device->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_presenter.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_presenter.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_presenter.device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_presenter.device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_presenter.device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_presenter.device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
    g_presenter.device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
    g_presenter.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_presenter.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_presenter.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_presenter.device->SetFVF(kFrameQuadFvf);
    g_presenter.device->SetTexture(0, tex);
    g_presenter.device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(FrameQuadVertex));
    g_presenter.device->SetTexture(0, nullptr);
}

int ComputePrescaleFactor(int srcW, int srcH, int dstW, int dstH)
{
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return 1;
    int pw = (dstW + srcW - 1) / srcW;
    int ph = (dstH + srcH - 1) / srcH;
    int p = (pw > ph) ? pw : ph;
    if (p < 1) p = 1;
    if (p > 6) p = 6;
    return p;
}

void RenderFrameQuad(const EfzDx9PresentParams &params)
{
    const float dx = static_cast<float>(params.dstX);
    const float dy = static_cast<float>(params.dstY);
    const float dw = static_cast<float>(params.dstW);
    const float dh = static_cast<float>(params.dstH);

    const int prescale = ComputePrescaleFactor(params.width, params.height, params.dstW, params.dstH);
    if (prescale > 1 && g_presenter.frameTexture != nullptr)
    {
        const int sw = params.width * prescale;
        const int sh = params.height * prescale;
        if (EnsureScaleTexture(sw, sh))
        {
            IDirect3DSurface9 *backRT = nullptr;
            IDirect3DSurface9 *scaleRT = nullptr;
            if (SUCCEEDED(g_presenter.device->GetRenderTarget(0, &backRT))
                && SUCCEEDED(g_presenter.scaleTexture->GetSurfaceLevel(0, &scaleRT)))
            {
                g_presenter.device->SetRenderTarget(0, scaleRT);
                DrawTexturedQuad(g_presenter.frameTexture, 0.0f, 0.0f,
                                 static_cast<float>(sw), static_cast<float>(sh), D3DTEXF_POINT);
                g_presenter.device->SetRenderTarget(0, backRT);
                DrawTexturedQuad(g_presenter.scaleTexture, dx, dy, dw, dh, D3DTEXF_LINEAR);
                if (scaleRT) scaleRT->Release();
                if (backRT) backRT->Release();
                return;
            }
            if (scaleRT) scaleRT->Release();
            if (backRT) backRT->Release();
        }
    }

    const DWORD filter = (params.linearScale != nullptr && *params.linearScale != FALSE)
        ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    DrawTexturedQuad(g_presenter.frameTexture, dx, dy, dw, dh, filter);
}

// Keep these ids in sync with the mod's exported settings API.
enum SettingBoolId
{
    SettingBoolApplyRemotePalettes = 0,
    SettingBoolNonArcadeRematch = 1,
    SettingBoolOfflineVsHuman = 2,
    SettingBoolDebugMenu = 3,
    SettingBoolEnableHolePunch = 4,
    SettingBoolEnableUpnp = 5,
    SettingBoolFileLogging = 6,
    SettingBoolConsole = 7,
    SettingBoolPreserveModLog = 8,
    SettingBoolReservedLegacyLogs = 9,
    SettingBoolTlsVerify = 10,
    SettingBoolForceWinInet = 11,
    SettingBoolForceEmbeddedTls = 12,
    SettingBoolHideEmptySets = 13,
    SettingBoolHarnessShm = 14,
    SettingBoolStressTest = 15,
    SettingBoolHudPreview = 16,
    SettingBoolAllowPracticeKeys = 17,
    SettingBoolHudRollbackMetrics = 18,
    SettingBoolDeepTelemetry = 19,
};

enum SettingIntId
{
    SettingIntDefaultPort = 0,
    SettingIntInputDelayP1 = 1,
    SettingIntInputDelayP2 = 2,
    SettingIntBgmVolume = 3,
    SettingIntSeVolume = 4,
    SettingIntSavestateSaveKey = 5,
    SettingIntSavestateLoadKey = 6,
    SettingIntPlayerSwapKey = 7,
    SettingIntFramestepPauseKey = 8,
    SettingIntFramestepStepKey = 9,
    SettingIntDelayDecreaseKey = 10,
    SettingIntDelayIncreaseKey = 11,
    SettingIntDelayPlayerToggleKey = 12,
    SettingIntCollisionHitKey = 13,
    SettingIntCollisionHurtKey = 14,
    SettingIntCollisionBoxKey = 15,
    SettingIntHudFontScale = 18,
    SettingIntHudPosBase = 19,
};

enum SettingStringId
{
    SettingStringNickname = 0,
    SettingStringLastJoinAddress = 1,
    SettingStringHolePunchServer = 2,
    SettingStringBaseUrl = 3,
    SettingStringProxyBaseUrl = 4,
};

#pragma pack(push, 1)
struct BmeSessionStatus
{
    int32_t  phase;
    int32_t  role;
    int32_t  pingMs;
    int32_t  delayFrames;
    int32_t  activePlayer;
    int32_t  p1Wins;
    int32_t  p2Wins;
    uint16_t port;
    uint16_t localPort;
    uint32_t phaseTickMs;
    char     address[64];
    char     nickname[64];
    char     p1Name[64];
    char     p2Name[64];
    char     phaseName[32];
    char     errorMsg[128];
};

struct BmeDelayMetrics
{
    int32_t serial;
    int32_t averagePingMs;
    int32_t minPingMs;
    int32_t maxPingMs;
    int32_t recommendedDelay;
    int32_t minDelay;
    int32_t maxDelay;
};

struct BmeMatchStats
{
    uint64_t localFrame;
    uint64_t peerFrame;
    int64_t  inputLead;
    uint64_t mispredictCount;
    uint64_t rollbackRequests;
    uint64_t rollbackExecuted;
    uint64_t rollbackAborted;
    uint64_t framesReplayed;
    uint32_t lastReplayLength;
    uint32_t _pad0;
};
#pragma pack(pop)

struct ModApi
{
    HMODULE handle = nullptr;
    bool probed = false;

    using IsAvailableFn = int (*)(void);
    using SetBotActiveFn = void (*)(int, int, int);
    using IsBotActiveFn = int (*)(int);
    using SetLoggingFn = void (*)(int);
    using GetLastFrameFn = int (*)(uint64_t*, uint32_t*, uint8_t*, uint8_t*);
    using GetInputStatusFn = void (*)(int, uint8_t*, uint8_t*);

    using SessionStartFn = int (*)(int, uint16_t, const char*, const char*);
    using SessionCancelFn = void (*)(const char*);
    using SessionApplyDelayFn = int (*)(int);
    using SessionGetStatusFn = int (*)(BmeSessionStatus*);
    using SessionGetDelayMetricsFn = int (*)(BmeDelayMetrics*);
    using SessionGetRecentEventsFn = int (*)(char*, int);
    using SessionGetMatchStatsFn = int (*)(BmeMatchStats*);
    using SessionGetEndReasonNameFn = const char* (*)(void);

    using SettingsVoidFn = void (*)(void);
    using SettingsGetBoolFn = int (*)(int);
    using SettingsSetBoolFn = int (*)(int, int);
    using SettingsGetIntFn = int (*)(int);
    using SettingsSetIntFn = int (*)(int, int);
    using SettingsGetStringFn = int (*)(int, char*, int);
    using SettingsSetStringFn = int (*)(int, const char*);

    IsAvailableFn botAvailable = nullptr;
    SetBotActiveFn setBotActive = nullptr;
    IsBotActiveFn isBotActive = nullptr;
    SetLoggingFn setBotLogging = nullptr;
    IsAvailableFn isBotLogging = nullptr;
    GetLastFrameFn getLastFrame = nullptr;
    GetInputStatusFn getInputStatus = nullptr;

    IsAvailableFn sessionAvailable = nullptr;
    SessionStartFn sessionStart = nullptr;
    SessionCancelFn sessionCancel = nullptr;
    SessionApplyDelayFn sessionApplyDelay = nullptr;
    SessionGetStatusFn sessionGetStatus = nullptr;
    SessionGetDelayMetricsFn sessionGetDelayMetrics = nullptr;
    SessionGetRecentEventsFn sessionGetRecentEvents = nullptr;
    SessionGetMatchStatsFn sessionGetMatchStats = nullptr;
    SessionGetEndReasonNameFn sessionGetEndReasonName = nullptr;

    IsAvailableFn settingsAvailable = nullptr;
    SettingsVoidFn settingsReload = nullptr;
    SettingsVoidFn settingsSave = nullptr;
    SettingsGetBoolFn settingsGetBool = nullptr;
    SettingsSetBoolFn settingsSetBool = nullptr;
    SettingsGetIntFn settingsGetInt = nullptr;
    SettingsSetIntFn settingsSetInt = nullptr;
    SettingsGetStringFn settingsGetString = nullptr;
    SettingsSetStringFn settingsSetString = nullptr;
};

struct DebugMenuEditState
{
    bool loadedFromSettings = false;
    char nickname[64] = "Player";
    char joinAddress[128] = "127.0.0.1";
    char holePunchServer[128] = "";
    char baseUrl[256] = "";
    char proxyBaseUrl[256] = "";
    int defaultPort = 10800;
    int sessionDelay = 2;
};

template <typename T>
T ResolveProc(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

ModApi& GetModApi()
{
    static ModApi api = {};
    HMODULE module = GetModuleHandleA("efz_bme_netplay.dll");
    if (api.handle != module)
    {
        api = {};
        api.handle = module;
    }

    if (module == nullptr || api.probed)
    {
        return api;
    }

    api.probed = true;
    api.botAvailable = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_BotMatch_IsAvailable");
    api.setBotActive = ResolveProc<ModApi::SetBotActiveFn>(module, "EFZBME_BotMatch_SetBotActive");
    api.isBotActive = ResolveProc<ModApi::IsBotActiveFn>(module, "EFZBME_BotMatch_IsBotActive");
    api.setBotLogging = ResolveProc<ModApi::SetLoggingFn>(module, "EFZBME_BotMatch_SetLoggingEnabled");
    api.isBotLogging = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_BotMatch_IsLoggingEnabled");
    api.getLastFrame = ResolveProc<ModApi::GetLastFrameFn>(module, "EFZBME_BotMatch_GetLastFrame");
    api.getInputStatus = ResolveProc<ModApi::GetInputStatusFn>(module, "EFZBME_BotMatch_GetInputStatus");

    api.sessionAvailable = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_Session_IsAvailable");
    api.sessionStart = ResolveProc<ModApi::SessionStartFn>(module, "EFZBME_Session_Start");
    api.sessionCancel = ResolveProc<ModApi::SessionCancelFn>(module, "EFZBME_Session_Cancel");
    api.sessionApplyDelay = ResolveProc<ModApi::SessionApplyDelayFn>(module, "EFZBME_Session_ApplyDelay");
    api.sessionGetStatus = ResolveProc<ModApi::SessionGetStatusFn>(module, "EFZBME_Session_GetStatus");
    api.sessionGetDelayMetrics = ResolveProc<ModApi::SessionGetDelayMetricsFn>(module, "EFZBME_Session_GetDelayMetrics");
    api.sessionGetRecentEvents = ResolveProc<ModApi::SessionGetRecentEventsFn>(module, "EFZBME_Session_GetRecentEvents");
    api.sessionGetMatchStats = ResolveProc<ModApi::SessionGetMatchStatsFn>(module, "EFZBME_Session_GetMatchStats");
    api.sessionGetEndReasonName = ResolveProc<ModApi::SessionGetEndReasonNameFn>(module, "EFZBME_Session_GetEndReasonName");

    api.settingsAvailable = ResolveProc<ModApi::IsAvailableFn>(module, "EFZBME_Settings_IsAvailable");
    api.settingsReload = ResolveProc<ModApi::SettingsVoidFn>(module, "EFZBME_Settings_Reload");
    api.settingsSave = ResolveProc<ModApi::SettingsVoidFn>(module, "EFZBME_Settings_Save");
    api.settingsGetBool = ResolveProc<ModApi::SettingsGetBoolFn>(module, "EFZBME_Settings_GetBool");
    api.settingsSetBool = ResolveProc<ModApi::SettingsSetBoolFn>(module, "EFZBME_Settings_SetBool");
    api.settingsGetInt = ResolveProc<ModApi::SettingsGetIntFn>(module, "EFZBME_Settings_GetInt");
    api.settingsSetInt = ResolveProc<ModApi::SettingsSetIntFn>(module, "EFZBME_Settings_SetInt");
    api.settingsGetString = ResolveProc<ModApi::SettingsGetStringFn>(module, "EFZBME_Settings_GetString");
    api.settingsSetString = ResolveProc<ModApi::SettingsSetStringFn>(module, "EFZBME_Settings_SetString");
    return api;
}

bool SettingsReady(const ModApi& api)
{
    return api.settingsAvailable != nullptr
        && api.settingsAvailable() != 0
        && api.settingsGetBool != nullptr
        && api.settingsSetBool != nullptr
        && api.settingsGetInt != nullptr
        && api.settingsSetInt != nullptr;
}

bool SessionReady(const ModApi& api)
{
    return api.sessionAvailable != nullptr
        && api.sessionAvailable() != 0
        && api.sessionGetStatus != nullptr;
}

bool BotReady(const ModApi& api)
{
    return api.botAvailable != nullptr
        && api.botAvailable() != 0
        && api.isBotActive != nullptr
        && api.setBotActive != nullptr;
}

void LoadStringSetting(const ModApi& api, int settingId, char* dst, int cap)
{
    if (api.settingsGetString != nullptr && dst != nullptr && cap > 0)
    {
        api.settingsGetString(settingId, dst, cap);
        dst[cap - 1] = '\0';
    }
}

void LoadEditStateFromSettings(const ModApi& api, DebugMenuEditState& edit)
{
    if (!SettingsReady(api))
    {
        return;
    }

    LoadStringSetting(api, SettingStringNickname, edit.nickname, sizeof(edit.nickname));
    LoadStringSetting(api, SettingStringLastJoinAddress, edit.joinAddress, sizeof(edit.joinAddress));
    LoadStringSetting(api, SettingStringHolePunchServer, edit.holePunchServer, sizeof(edit.holePunchServer));
    LoadStringSetting(api, SettingStringBaseUrl, edit.baseUrl, sizeof(edit.baseUrl));
    LoadStringSetting(api, SettingStringProxyBaseUrl, edit.proxyBaseUrl, sizeof(edit.proxyBaseUrl));
    edit.defaultPort = api.settingsGetInt(SettingIntDefaultPort);
    if (edit.defaultPort <= 0)
    {
        edit.defaultPort = 10800;
    }
    edit.loadedFromSettings = true;
}

bool DrawSettingCheckbox(const ModApi& api, int settingId, const char* label)
{
    const bool ready = SettingsReady(api);
    bool value = ready ? (api.settingsGetBool(settingId) != 0) : false;
    ImGui::BeginDisabled(!ready);
    const bool changed = ImGui::Checkbox(label, &value);
    if (changed)
    {
        api.settingsSetBool(settingId, value ? 1 : 0);
    }
    ImGui::EndDisabled();
    return changed;
}

void DrawSettingSlider(const ModApi& api, int settingId, const char* label, int minValue, int maxValue, const char* format)
{
    const bool ready = SettingsReady(api);
    int value = ready ? api.settingsGetInt(settingId) : 0;
    ImGui::BeginDisabled(!ready);
    if (ImGui::SliderInt(label, &value, minValue, maxValue, format))
    {
        api.settingsSetInt(settingId, value);
    }
    ImGui::EndDisabled();
}

void DrawHotkeyInt(const ModApi& api, int settingId, const char* label)
{
    const bool ready = SettingsReady(api);
    int value = ready ? api.settingsGetInt(settingId) : 0;
    ImGui::BeginDisabled(!ready);
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.0f);
    ImGui::SetNextItemWidth(90.0f);
    int edited = value;
    if (ImGui::InputInt("VK", &edited, 1, 16, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (edited < 0) edited = 0;
        if (edited > 0xFE) edited = 0xFE;
        api.settingsSetInt(settingId, edited);
        value = edited;
    }
    ImGui::SameLine();
    ImGui::Text("0x%02X", static_cast<unsigned>(value & 0xFF));
    ImGui::PopID();
    ImGui::EndDisabled();
}

void DrawHudPosRow(const ModApi& api, int elementIndex, const char* label)
{
    const bool ready = SettingsReady(api);
    const int idX = SettingIntHudPosBase + elementIndex * 2;
    const int idY = idX + 1;
    int x = ready ? api.settingsGetInt(idX) : -1;
    int y = ready ? api.settingsGetInt(idY) : -1;

    ImGui::BeginDisabled(!ready);
    ImGui::PushID(elementIndex);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::InputInt("X", &x, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (x < -1) x = -1;
        api.settingsSetInt(idX, x);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::InputInt("Y", &y, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (y < -1) y = -1;
        api.settingsSetInt(idY, y);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset"))
    {
        api.settingsSetInt(idX, -1);
        api.settingsSetInt(idY, -1);
    }
    ImGui::PopID();
    ImGui::EndDisabled();
}

const char* RoleName(int role)
{
    switch (role)
    {
    case 0: return "Host";
    case 1: return "Join";
    case 2: return "Spectate";
    default: return "None";
    }
}

void DrawDisplayTab(const EfzDx9PresentParams& params)
{
    ImGui::Text("Present: %lu", static_cast<unsigned long>(params.presentSerial));
    ImGui::Text("Focus serial: %lu", static_cast<unsigned long>(params.focusSerial));
    ImGui::Text("Source: %d x %d (pitch %d)", params.width, params.height, params.pitch);
    ImGui::Text("Client: %d x %d", params.clientWidth, params.clientHeight);
    ImGui::Text("Viewport: %d, %d -> %d x %d", params.dstX, params.dstY, params.dstW, params.dstH);
    ImGui::Text("Mode: %s", params.borderless ? "Borderless" : "Windowed");

    if (params.linearScale != nullptr)
    {
        bool linearScale = *params.linearScale != FALSE;
        if (ImGui::Checkbox("Linear scaling", &linearScale))
        {
            *params.linearScale = linearScale ? TRUE : FALSE;
        }
    }

    bool borderless = params.borderless != FALSE;
    if (ImGui::Checkbox("Borderless fullscreen", &borderless) && params.toggleBorderless != nullptr)
    {
        params.toggleBorderless(params.toggleBorderlessUserData);
    }

    ImGui::Checkbox("Dear ImGui demo", &g_presenter.showDemoWindow);
}

void DrawSessionTab(const ModApi& api, DebugMenuEditState& edit)
{
    const bool ready = SessionReady(api);
    if (!ready)
    {
        ImGui::TextDisabled("Session API unavailable.");
    }

    BmeSessionStatus status = {};
    if (ready && api.sessionGetStatus(&status) != 0)
    {
        ImGui::Text("Phase: %s", status.phaseName[0] ? status.phaseName : "?");
        ImGui::SameLine(210.0f);
        ImGui::Text("Role: %s", RoleName(status.role));
        ImGui::Text("Ping: %d ms", status.pingMs);
        ImGui::SameLine(150.0f);
        ImGui::Text("Delay: %d F", status.delayFrames);
        ImGui::SameLine(270.0f);
        ImGui::Text("Wins: %d - %d", status.p1Wins, status.p2Wins);
        if (status.port != 0 || status.localPort != 0)
        {
            ImGui::Text("Target port: %u", static_cast<unsigned>(status.port));
            ImGui::SameLine(180.0f);
            ImGui::Text("Local UDP: %u", static_cast<unsigned>(status.localPort));
        }
        if (status.errorMsg[0] != '\0')
        {
            ImGui::TextWrapped("Error: %s", status.errorMsg);
        }
    }

    ImGui::SeparatorText("Connect");
    ImGui::BeginDisabled(!ready || api.sessionStart == nullptr);
    ImGui::InputText("Nickname", edit.nickname, sizeof(edit.nickname));
    ImGui::InputText("Address", edit.joinAddress, sizeof(edit.joinAddress));
    ImGui::InputInt("Port", &edit.defaultPort);
    if (edit.defaultPort < 1) edit.defaultPort = 1;
    if (edit.defaultPort > 65535) edit.defaultPort = 65535;

    if (ImGui::Button("Host"))
    {
        api.sessionStart(0, static_cast<uint16_t>(edit.defaultPort), "", edit.nickname);
    }
    ImGui::SameLine();
    if (ImGui::Button("Join"))
    {
        api.sessionStart(1, static_cast<uint16_t>(edit.defaultPort), edit.joinAddress, edit.nickname);
    }
    ImGui::SameLine();
    if (ImGui::Button("Spectate"))
    {
        api.sessionStart(2, static_cast<uint16_t>(edit.defaultPort), edit.joinAddress, edit.nickname);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready || api.sessionCancel == nullptr);
    if (ImGui::Button("Cancel"))
    {
        api.sessionCancel("f1-menu");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Delay");
    BmeDelayMetrics metrics = {};
    if (ready && api.sessionGetDelayMetrics != nullptr && api.sessionGetDelayMetrics(&metrics) != 0)
    {
        ImGui::Text("RTT avg/min/max: %d / %d / %d ms", metrics.averagePingMs, metrics.minPingMs, metrics.maxPingMs);
        ImGui::Text("Recommended: %d F", metrics.recommendedDelay);
        if (edit.sessionDelay < metrics.minDelay || edit.sessionDelay > metrics.maxDelay)
        {
            edit.sessionDelay = metrics.recommendedDelay;
        }
        ImGui::BeginDisabled(api.sessionApplyDelay == nullptr);
        if (ImGui::SliderInt("Gameplay delay", &edit.sessionDelay, metrics.minDelay, metrics.maxDelay, "%d F"))
        {
            api.sessionApplyDelay(edit.sessionDelay);
        }
        ImGui::EndDisabled();
    }
    else
    {
        ImGui::TextDisabled("Delay metrics unavailable.");
    }

    ImGui::SeparatorText("Runtime");
    BmeMatchStats stats = {};
    if (ready && api.sessionGetMatchStats != nullptr && api.sessionGetMatchStats(&stats) != 0)
    {
        ImGui::Text("Frames local/peer: %llu / %llu",
            static_cast<unsigned long long>(stats.localFrame),
            static_cast<unsigned long long>(stats.peerFrame));
        ImGui::Text("Lead: %lld    Mispredicts: %llu",
            static_cast<long long>(stats.inputLead),
            static_cast<unsigned long long>(stats.mispredictCount));
        ImGui::Text("Rollback req/exe/abort: %llu / %llu / %llu",
            static_cast<unsigned long long>(stats.rollbackRequests),
            static_cast<unsigned long long>(stats.rollbackExecuted),
            static_cast<unsigned long long>(stats.rollbackAborted));
        ImGui::Text("Replayed: %llu    Last: %u",
            static_cast<unsigned long long>(stats.framesReplayed),
            static_cast<unsigned>(stats.lastReplayLength));
    }

    if (api.sessionGetEndReasonName != nullptr)
    {
        const char* reason = api.sessionGetEndReasonName();
        ImGui::Text("End reason: %s", reason != nullptr ? reason : "?");
    }

    ImGui::SeparatorText("Recent events");
    ImGui::BeginChild("recent_events", ImVec2(0.0f, 145.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    static char events[8192] = {};
    events[0] = '\0';
    if (ready && api.sessionGetRecentEvents != nullptr)
    {
        api.sessionGetRecentEvents(events, static_cast<int>(sizeof(events)));
    }
    if (events[0] != '\0')
    {
        ImGui::TextUnformatted(events);
    }
    else
    {
        ImGui::TextDisabled("No events.");
    }
    ImGui::EndChild();
}

void DrawSettingsTab(const ModApi& api, DebugMenuEditState& edit)
{
    if (!SettingsReady(api))
    {
        ImGui::TextDisabled("Settings API unavailable.");
        return;
    }

    if (ImGui::Button("Reload from INI"))
    {
        if (api.settingsReload != nullptr)
        {
            api.settingsReload();
        }
        LoadEditStateFromSettings(api, edit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        if (api.settingsSave != nullptr)
        {
            api.settingsSave();
        }
    }

    if (ImGui::CollapsingHeader("Profile", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputText("Nickname", edit.nickname, sizeof(edit.nickname));
        if (ImGui::Button("Apply profile"))
        {
            if (api.settingsSetString != nullptr)
            {
                api.settingsSetString(SettingStringNickname, edit.nickname);
            }
        }
    }

    if (ImGui::CollapsingHeader("Netplay", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSettingCheckbox(api, SettingBoolApplyRemotePalettes, "Apply remote palettes");
        DrawSettingCheckbox(api, SettingBoolNonArcadeRematch, "Non-arcade rematch");
        DrawSettingCheckbox(api, SettingBoolAllowPracticeKeys, "Allow practice keys");
        DrawSettingCheckbox(api, SettingBoolDeepTelemetry, "Deep telemetry logs");
    }

    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSettingSlider(api, SettingIntBgmVolume, "BGM volume", 0, 100, "%d%%");
        DrawSettingSlider(api, SettingIntSeVolume, "SE volume", 0, 100, "%d%%");
    }

    if (ImGui::CollapsingHeader("HUD", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSettingSlider(api, SettingIntHudFontScale, "HUD font scale", 50, 300, "%d%%");
        DrawSettingCheckbox(api, SettingBoolHudPreview, "Preview HUD offline");
        DrawSettingCheckbox(api, SettingBoolHudRollbackMetrics, "Rollback/desync line");

        // -1 = auto. Right-side elements anchor their right edge, left-side their left.
        if (ImGui::TreeNodeEx("Layout (per element)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextDisabled("Virtual 640x480. -1 = auto. Enter to apply.");
            DrawHudPosRow(api, 0, "P1 name");
            DrawHudPosRow(api, 1, "P2 name");
            DrawHudPosRow(api, 2, "Ping");
            DrawHudPosRow(api, 3, "Delay");
            DrawHudPosRow(api, 4, "Rollback");
            DrawHudPosRow(api, 5, "Status");
            DrawHudPosRow(api, 6, "Practice");
            DrawHudPosRow(api, 7, "Toasts");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Network"))
    {
        ImGui::InputText("Join address", edit.joinAddress, sizeof(edit.joinAddress));
        ImGui::InputText("Hole-punch server", edit.holePunchServer, sizeof(edit.holePunchServer));
        ImGui::InputText("Base URL", edit.baseUrl, sizeof(edit.baseUrl));
        ImGui::InputText("Proxy Base URL", edit.proxyBaseUrl, sizeof(edit.proxyBaseUrl));
        ImGui::InputInt("Default port", &edit.defaultPort);
        if (edit.defaultPort < 1) edit.defaultPort = 1;
        if (edit.defaultPort > 65535) edit.defaultPort = 65535;
        DrawSettingCheckbox(api, SettingBoolEnableHolePunch, "Hole punch");
        DrawSettingCheckbox(api, SettingBoolEnableUpnp, "UPnP");
        if (ImGui::Button("Apply network"))
        {
            if (api.settingsSetString != nullptr)
            {
                api.settingsSetString(SettingStringLastJoinAddress, edit.joinAddress);
                api.settingsSetString(SettingStringHolePunchServer, edit.holePunchServer);
                api.settingsSetString(SettingStringBaseUrl, edit.baseUrl);
                api.settingsSetString(SettingStringProxyBaseUrl, edit.proxyBaseUrl);
            }
            if (api.settingsSetInt != nullptr)
            {
                api.settingsSetInt(SettingIntDefaultPort, edit.defaultPort);
            }
        }
    }

    if (ImGui::CollapsingHeader("Logging"))
    {
        DrawSettingCheckbox(api, SettingBoolFileLogging, "File logging");
        DrawSettingCheckbox(api, SettingBoolConsole, "Console");
        DrawSettingCheckbox(api, SettingBoolPreserveModLog, "Preserve mod log");
    }

    if (ImGui::CollapsingHeader("Transport"))
    {
        DrawSettingCheckbox(api, SettingBoolTlsVerify, "TLS verify");
        DrawSettingCheckbox(api, SettingBoolForceWinInet, "Force WinInet");
        DrawSettingCheckbox(api, SettingBoolForceEmbeddedTls, "Force embedded TLS");
    }

    if (ImGui::CollapsingHeader("Hotkeys"))
    {
        DrawHotkeyInt(api, SettingIntSavestateSaveKey, "Savestate save");
        DrawHotkeyInt(api, SettingIntSavestateLoadKey, "Savestate load");
        DrawHotkeyInt(api, SettingIntPlayerSwapKey, "Player swap");
        DrawHotkeyInt(api, SettingIntFramestepPauseKey, "Framestep pause");
        DrawHotkeyInt(api, SettingIntFramestepStepKey, "Framestep step");
        DrawHotkeyInt(api, SettingIntDelayDecreaseKey, "Delay decrease");
        DrawHotkeyInt(api, SettingIntDelayIncreaseKey, "Delay increase");
        DrawHotkeyInt(api, SettingIntDelayPlayerToggleKey, "Delay player toggle");
        DrawHotkeyInt(api, SettingIntCollisionHitKey, "Hitbox layer");
        DrawHotkeyInt(api, SettingIntCollisionHurtKey, "Hurtbox layer");
        DrawHotkeyInt(api, SettingIntCollisionBoxKey, "Collision layer");
    }
}

void DrawTestingTab(const ModApi& api)
{
    DrawSettingCheckbox(api, SettingBoolHarnessShm, "Harness shared memory");
    DrawSettingCheckbox(api, SettingBoolStressTest, "Determinism stress test");

    ImGui::SeparatorText("Bot match");
    const bool ready = BotReady(api);
    if (!ready)
    {
        ImGui::TextDisabled("Bot API unavailable.");
        return;
    }

    static const char* kPersonalities[] = { "Neutral Poker", "Block Trainer", "Idle Dummy", "Smart Fighter" };
    static int s_p1Personality = 0;
    static int s_p2Personality = 1;

    const bool p1Active = api.isBotActive(0) != 0;
    const bool p2Active = api.isBotActive(1) != 0;

    ImGui::PushID("p1_bot");
    ImGui::Combo("P1 personality", &s_p1Personality, kPersonalities, IM_ARRAYSIZE(kPersonalities));
    bool p1Toggle = p1Active;
    if (ImGui::Checkbox("P1 bot active", &p1Toggle))
    {
        api.setBotActive(0, p1Toggle ? 1 : 0, s_p1Personality);
    }
    ImGui::PopID();

    ImGui::PushID("p2_bot");
    ImGui::Combo("P2 personality", &s_p2Personality, kPersonalities, IM_ARRAYSIZE(kPersonalities));
    bool p2Toggle = p2Active;
    if (ImGui::Checkbox("P2 bot active", &p2Toggle))
    {
        api.setBotActive(1, p2Toggle ? 1 : 0, s_p2Personality);
    }
    ImGui::PopID();

    if (api.isBotLogging != nullptr && api.setBotLogging != nullptr)
    {
        bool logToggle = api.isBotLogging() != 0;
        if (ImGui::Checkbox("Desync CSV logging", &logToggle))
        {
            api.setBotLogging(logToggle ? 1 : 0);
        }
    }

    if (api.getLastFrame != nullptr)
    {
        uint64_t lastFrame = 0;
        uint32_t lastHash = 0;
        uint8_t p1In = 0;
        uint8_t p2In = 0;
        const int valid = api.getLastFrame(&lastFrame, &lastHash, &p1In, &p2In);
        if (valid)
        {
            ImGui::Text("Frame %llu  hash 0x%08X",
                static_cast<unsigned long long>(lastFrame),
                static_cast<unsigned>(lastHash));
            ImGui::Text("P1 in: 0x%02X    P2 in: 0x%02X",
                static_cast<unsigned>(p1In),
                static_cast<unsigned>(p2In));
        }
        else
        {
            ImGui::TextDisabled("No sampled frame.");
        }
    }

    if (api.getInputStatus != nullptr)
    {
        static const char* kSourceNames[] = { "Idle", "Local", "Bot", "Replay", "Remote" };
        uint8_t resolved = 0;
        uint8_t source = 0;
        api.getInputStatus(0, &resolved, &source);
        const char* srcName = (source < IM_ARRAYSIZE(kSourceNames)) ? kSourceNames[source] : "?";
        ImGui::Text("P1 resolved: 0x%02X (%s)", resolved, srcName);
        api.getInputStatus(1, &resolved, &source);
        srcName = (source < IM_ARRAYSIZE(kSourceNames)) ? kSourceNames[source] : "?";
        ImGui::Text("P2 resolved: 0x%02X (%s)", resolved, srcName);
    }
}

void BuildDebugMenu(const EfzDx9PresentParams &params)
{
    ModApi& api = GetModApi();
    static DebugMenuEditState s_edit = {};
    if (!s_edit.loadedFromSettings && SettingsReady(api))
    {
        LoadEditStateFromSettings(api, s_edit);
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Behind the Clouds Debug", &g_presenter.menuVisible, ImGuiWindowFlags_NoCollapse))
    {
        if (api.handle == nullptr)
        {
            ImGui::TextDisabled("efz_bme_netplay.dll not loaded.");
        }

        if (ImGui::BeginTabBar("debug_tabs"))
        {
            if (ImGui::BeginTabItem("Display"))
            {
                DrawDisplayTab(params);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Session"))
            {
                DrawSessionTab(api, s_edit);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings"))
            {
                DrawSettingsTab(api, s_edit);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Testing"))
            {
                DrawTestingTab(api);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (g_presenter.showDemoWindow)
    {
        ImGui::ShowDemoWindow(&g_presenter.showDemoWindow);
    }
}

bool IsInputMessage(UINT msg)
{
    switch (msg)
    {
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSCHAR:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_SETCURSOR:
        return true;
    default:
        return false;
    }
}
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern "C" BOOL EFZDX9_Presenter_HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *outResult)
{
    if (TrainingMenuShouldDraw() && g_presenter.trainingMenuHandleMessage != nullptr)
    {
        if (g_presenter.trainingMenuHandleMessage(hwnd, msg, wParam, lParam, outResult, g_presenter.trainingMenuUserData))
        {
            return TRUE;
        }
    }

    if (!g_presenter.menuVisible)
    {
        return FALSE;
    }

    if (g_presenter.imguiInitialized && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        if (outResult != nullptr)
        {
            *outResult = 1;
        }
        return TRUE;
    }

    if (IsInputMessage(msg))
    {
        if (outResult != nullptr)
        {
            *outResult = 0;
        }
        return TRUE;
    }

    return FALSE;
}

extern "C" BOOL EFZDX9_Presenter_IsMenuVisible(void)
{
    return g_presenter.menuVisible ? TRUE : FALSE;
}

extern "C" BOOL EFZDX9_Presenter_ShouldSuppressGameInput(void)
{
    return (g_presenter.menuVisible || TrainingMenuShouldDraw()) ? TRUE : FALSE;
}

extern "C" BOOL EFZDX9_Presenter_Present(const EfzDx9PresentParams *params)
{
    if (params == nullptr || params->window == nullptr || params->pixels == nullptr
        || params->width <= 0 || params->height <= 0 || params->pitch <= 0)
    {
        return FALSE;
    }

    if (!PresenterNeedsAnyOverlay() && g_presenter.device == nullptr)
    {
        return FALSE;
    }

    g_presenter.appActive = params->appActive != FALSE;
    if (!EnsureDevice(*params) || !EnsureFrameTexture(params->width, params->height) || !UploadIndexedFrame(*params))
    {
        return FALSE;
    }

    g_presenter.device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(g_presenter.device->BeginScene()))
    {
        return FALSE;
    }

    RenderFrameQuad(*params);

    if (g_presenter.menuVisible)
    {
        RenderTrainingMenu(params);
    }

    if (PresenterNeedsImGuiPath())
    {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawNetplayHud(*params);
        DrawGenericOverlay(*params);
        if (g_presenter.menuVisible)
        {
            BuildDebugMenu(*params);
        }
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    if (!g_presenter.menuVisible)
    {
        RenderTrainingMenu(params);
    }

    g_presenter.device->EndScene();
    const HRESULT presentHr = g_presenter.device->Present(nullptr, nullptr, nullptr, nullptr);
    if (presentHr == D3DERR_DEVICELOST)
    {
        g_presenter.deviceLost = true;
        return FALSE;
    }

    return SUCCEEDED(presentHr) ? TRUE : FALSE;
}

extern "C" void EFZDX9_Presenter_SetActive(BOOL active)
{
    g_presenter.appActive = active != FALSE;
    if (g_presenter.imguiInitialized)
    {
        ImGui::GetIO().AddFocusEvent(active != FALSE);
    }
}

extern "C" void EFZDX9_Presenter_Shutdown(void)
{
    DestroyDevice();
    g_presenter.menuVisible = false;
    g_presenter.showDemoWindow = false;
    g_presenter.appActive = true;
}

extern "C" void EFZDX9_Presenter_ToggleMenu(void)
{
    g_presenter.menuVisible = !g_presenter.menuVisible;
}

extern "C" void __stdcall EFZDX9_RegisterNetplayHudProvider(
    EfzDx9NetplayHudProvider provider,
    void *userData)
{
    g_presenter.hudProvider = provider;
    g_presenter.hudProviderUserData = userData;
}

extern "C" void __stdcall EFZDX9_RegisterOverlayProvider(
    EfzDx9OverlayProvider provider,
    void *userData)
{
    g_presenter.overlayProvider = provider;
    g_presenter.overlayProviderUserData = userData;
}

extern "C" void __stdcall EFZDX9_RegisterTrainingMenuBridge(
    EfzDx9TrainingMenuRendererFn renderer,
    EfzDx9TrainingMenuVisibleFn isVisible,
    EfzDx9TrainingMenuHandleMessageFn handleMessage,
    EfzDx9TrainingMenuWantsDrawFn wantsDraw,
    EfzDx9TrainingMenuDeviceNotifyFn deviceNotify,
    void *userData)
{
    g_presenter.trainingMenuRenderer = renderer;
    g_presenter.trainingMenuVisible = isVisible;
    g_presenter.trainingMenuHandleMessage = handleMessage;
    g_presenter.trainingMenuWantsDraw = wantsDraw;
    g_presenter.trainingMenuDeviceNotify = deviceNotify;
    g_presenter.trainingMenuUserData = userData;
}
