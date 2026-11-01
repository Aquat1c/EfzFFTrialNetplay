#include "caster_ui.h"

#include "caster_app.h"
#include "caster_config.h"
#include "control_bindings.h"
#include "delay_recommendation.h"
#include "ip_info.h"
#include "session_log.h"
#include "util.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace trial_caster::ui
{
namespace
{

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_bannerTexture = nullptr;
bool g_running = false;
bool g_rendererBackendReady = false;
bool g_renderVsyncActive = false;
Uint64 g_nextUnthrottledFrameMs = 0u;
float g_uiScale = 1.0f;
float g_userScale = 1.0f;
float g_displayScale = 1.0f;
bool g_responsiveScalePending = false;
Uint64 g_lastWindowResizeMs = 0u;

constexpr float kDefaultWindowWidth = 760.0f;
constexpr float kDefaultWindowHeight = 560.0f;
constexpr float kMinimumWindowWidth = 570.0f;
constexpr float kMinimumWindowHeight = 420.0f;
constexpr Uint64 kResponsiveScaleSettleMs = 100u;
constexpr Uint64 kUnthrottledFrameIntervalMs = 16u;

ImFont* g_fontBody = nullptr;
ImFont* g_fontHeader = nullptr;
ImFont* g_fontStat = nullptr;
ImFont* g_fontSmall = nullptr;

const ImVec4 kAccent(0.412f, 0.922f, 0.922f, 1.00f);
const ImVec4 kAccentSoft(0.118f, 0.608f, 0.745f, 0.41f);
const ImVec4 kHudOk(0.32f, 0.92f, 0.56f, 1.00f);
const ImVec4 kHudWarn(1.00f, 0.74f, 0.26f, 1.00f);
const ImVec4 kHudBad(1.00f, 0.40f, 0.42f, 1.00f);
const ImVec4 kHudMuted(0.588f, 0.588f, 0.588f, 0.92f);
const ImVec4 kCardBg(0.0f, 0.0f, 0.0f, 0.80f);
const ImVec4 kCardBorder(1.0f, 1.0f, 1.0f, 0.34f);

float Ui(const float value)
{
    return value * g_uiScale;
}

float UiDimension(const float value)
{
    // Preserve ImGui's 0 (automatic) and -1 (fill-remaining) sentinels.
    return value > 0.0f ? Ui(value) : value;
}

ImVec2 UiSize(const float x, const float y)
{
    return ImVec2(UiDimension(x), UiDimension(y));
}

void UsableWindowBounds(int* const maximumWidth, int* const maximumHeight)
{
    int maxWidth = std::numeric_limits<int>::max();
    int maxHeight = std::numeric_limits<int>::max();
    if (g_window != nullptr)
    {
        SDL_Rect usable{};
        SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
        if (display == 0u)
        {
            display = SDL_GetPrimaryDisplay();
        }
        if (display != 0u && SDL_GetDisplayUsableBounds(display, &usable))
        {
            maxWidth = std::max(1, static_cast<int>(std::lround(usable.w * 0.92f)));
            maxHeight = std::max(1, static_cast<int>(std::lround(usable.h * 0.92f)));
        }
    }
    if (maximumWidth != nullptr)
    {
        *maximumWidth = maxWidth;
    }
    if (maximumHeight != nullptr)
    {
        *maximumHeight = maxHeight;
    }
}

void CenterWindowOnCurrentDisplay(const int windowWidth, const int windowHeight)
{
    if (g_window == nullptr || windowWidth <= 0 || windowHeight <= 0)
    {
        return;
    }
    SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
    if (display == 0u)
    {
        display = SDL_GetPrimaryDisplay();
    }
    SDL_Rect usable{};
    if (display == 0u || !SDL_GetDisplayUsableBounds(display, &usable))
    {
        return;
    }
    SDL_SetWindowPosition(
        g_window,
        usable.x + std::max(0, (usable.w - windowWidth) / 2),
        usable.y + std::max(0, (usable.h - windowHeight) / 2));
}

float EffectiveUiScaleForDisplay(const float requestedScale)
{
    int maximumWidth = 0;
    int maximumHeight = 0;
    UsableWindowBounds(&maximumWidth, &maximumHeight);

    const float requested = std::clamp(requestedScale, 0.75f, 4.0f);
    const float fitScale = std::min(
        static_cast<float>(maximumWidth) / kDefaultWindowWidth,
        static_cast<float>(maximumHeight) / kDefaultWindowHeight);

    // One factor for both axes: independent clamps distort on short high-DPI desktops.
    return std::max(0.50f, std::min(requested, fitScale));
}

float ResponsiveUiScaleForWindow(const float fallbackScale)
{
    if (g_window == nullptr)
    {
        return fallbackScale;
    }

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(g_window, &width, &height) || width <= 0 || height <= 0)
    {
        return fallbackScale;
    }

    const float fitScale = std::min(
        static_cast<float>(width) / kDefaultWindowWidth,
        static_cast<float>(height) / kDefaultWindowHeight);
    return std::clamp(fitScale, 0.50f, 4.0f);
}

void ScaledWindowLimits(
    const float,
    int* const minimumWidth,
    int* const minimumHeight,
    int* const maximumWidth,
    int* const maximumHeight)
{
    int maxWidth = 0;
    int maxHeight = 0;
    UsableWindowBounds(&maxWidth, &maxHeight);

    if (minimumWidth != nullptr)
    {
        *minimumWidth = std::max(
            1,
            std::min(maxWidth, static_cast<int>(std::lround(kMinimumWindowWidth))));
    }
    if (minimumHeight != nullptr)
    {
        *minimumHeight = std::max(
            1,
            std::min(maxHeight, static_cast<int>(std::lround(kMinimumWindowHeight))));
    }
    if (maximumWidth != nullptr)
    {
        *maximumWidth = maxWidth;
    }
    if (maximumHeight != nullptr)
    {
        *maximumHeight = maxHeight;
    }
}

void PhaseInfo(AppPhase phase, const char*& label, ImVec4& color);

bool ConfigurationIsEditable(const AppPhase phase)
{
    // Connections carry a frozen copy of match settings; keep the visible config in step.
    return phase == AppPhase::Idle || phase == AppPhase::Error;
}

char g_connectIp[64] = {};
char g_playerName[64] = "Player";

enum class LauncherPage
{
    Play,
    Practice,
    Controls,
    Settings,
};

LauncherPage g_page = LauncherPage::Play;
float g_pageHighlightY = 0.0f;
float g_pageContentOffset = 0.0f;
float g_pageContentAlpha = 1.0f;

enum class RebindTarget
{
    None,
    Keyboard,
    Gamepad,
};

enum class RebindSubject
{
    None,
    FightControl,
    Shortcut,
};

int g_rebindPlayer = -1;
int g_rebindSlot = -1;
RebindTarget g_rebindTarget = RebindTarget::None;
RebindSubject g_rebindSubject = RebindSubject::None;
trialnet::shortcuts::Action g_rebindShortcut = trialnet::shortcuts::Action::Count;
std::wstring g_controlBindingWarning;

bool SaveConfigOrWarn(CasterApp& app)
{
    if (app.Config().Save())
    {
        return true;
    }

    g_controlBindingWarning =
        L"Couldn't save launcher settings. Check that the game folder is writable.";
    app.AppendLog(g_controlBindingWarning);
    return false;
}

struct LauncherController
{
    SDL_Gamepad* gamepad = nullptr;
    SDL_Joystick* joystick = nullptr;
    bool ownsJoystick = false;
    char name[128] = {};
};

std::array<LauncherController, 2> g_launcherControllers{};
Uint64 g_nextLauncherControllerScanMs = 0;
Uint64 g_rebindStartedMs = 0;
bool g_rebindAxisArmed = false;

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
    if (written <= 1)
    {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

std::wstring Utf8ToWide(const char* const text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0u, text, -1, nullptr, 0);
    if (required <= 1)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        0u,
        text,
        -1,
        result.data(),
        required);
    if (written <= 1)
    {
        return {};
    }
    result.resize(static_cast<std::size_t>(written - 1));
    return result;
}

void CopyToBuffer(char* dst, std::size_t dstSize, const std::wstring& src)
{
    const std::string utf8 = WideToUtf8(src);
    std::snprintf(dst, dstSize, "%s", utf8.c_str());
}

void SyncConfigBuffers(CasterConfig& cfg)
{
    CopyToBuffer(g_playerName, sizeof(g_playerName), cfg.playerName);
    CopyToBuffer(g_connectIp, sizeof(g_connectIp), cfg.lastConnectIp);
}

void ApplyConfigBuffers(CasterConfig& cfg)
{
    auto narrowToWide = [](const char* text) -> std::wstring
    {
        return Utf8ToWide(text);
    };

    cfg.playerName = narrowToWide(g_playerName);
}

void LoadHudFonts(const float scale)
{
    ImGuiIO& io = ImGui::GetIO();
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = module != nullptr
        ? FindResourceA(module, "TRIALFONT", reinterpret_cast<LPCSTR>(RT_RCDATA)) : nullptr;
    HGLOBAL handle = resource != nullptr ? LoadResource(module, resource) : nullptr;
    const void* data = handle != nullptr ? LockResource(handle) : nullptr;
    const DWORD size = resource != nullptr ? SizeofResource(module, resource) : 0;

    auto addFace = [&](const float pixels) -> ImFont*
    {
        if (data == nullptr || size == 0)
        {
            return nullptr;
        }
        void* copy = IM_ALLOC(size);
        if (copy == nullptr)
        {
            return nullptr;
        }
        std::memcpy(copy, data, size);
        ImFontConfig config;
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.PixelSnapH = true;
        config.FontDataOwnedByAtlas = true;
        return io.Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(size), pixels, &config);
    };

    g_fontBody = addFace(13.0f * scale);
    g_fontSmall = addFace(11.0f * scale);
    g_fontStat = addFace(13.0f * scale);
    g_fontHeader = addFace(16.0f * scale);
    if (g_fontBody != nullptr)
    {
        io.FontDefault = g_fontBody;
    }
    else
    {
        g_fontBody = io.Fonts->AddFontDefault();
        io.FontDefault = g_fontBody;
    }
}

void ApplyCasterTheme(const float scale)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle{};
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.025f, 0.035f, 0.045f, 1.0f);
    colors[ImGuiCol_ChildBg] = kCardBg;
    colors[ImGuiCol_PopupBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.96f);
    colors[ImGuiCol_Border] = kCardBorder;
    colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.80f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    colors[ImGuiCol_FrameBgActive] = kAccentSoft;
    colors[ImGuiCol_TitleBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.88f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.92f);
    colors[ImGuiCol_Header] = kAccentSoft;
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.80f, 1.00f, 0.32f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.80f, 1.00f, 0.45f);
    colors[ImGuiCol_Button] = ImVec4(0.0f, 0.0f, 0.0f, 0.80f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    colors[ImGuiCol_ButtonActive] = kAccentSoft;
    colors[ImGuiCol_Tab] = ImVec4(0.0f, 0.0f, 0.0f, 0.82f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.16f, 0.80f, 1.00f, 0.30f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.14f, 0.22f, 0.32f, 1.0f);
    colors[ImGuiCol_TabSelectedOverline] = kAccent;
    colors[ImGuiCol_CheckMark] = kAccent;
    colors[ImGuiCol_SliderGrab] = kAccent;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.90f, 1.0f, 1.0f);
    colors[ImGuiCol_SeparatorHovered] = kAccent;
    colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    colors[ImGuiCol_TextDisabled] = kHudMuted;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.07f, 0.10f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.42f, 0.42f, 0.42f, 0.85f);

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 3.0f);
    style.TabBarBorderSize = 1.0f;
    style.GrabMinSize = 9.0f;
    style.ScrollbarSize = 11.0f;
    style.ScaleAllSizes(scale);
}

void ApplyUiScale(const float requestedScale, const bool resizeWindow)
{
    // Do not clamp responsive requests to 92% of desktop: maximized stays undersized.
    const float scale = std::clamp(requestedScale, 0.50f, 4.0f);
    if (std::abs(scale - g_uiScale) < 0.01f)
    {
        return;
    }

    const float previous = g_uiScale;
    if (g_rendererBackendReady)
    {
        ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_fontBody = nullptr;
    g_fontHeader = nullptr;
    g_fontStat = nullptr;
    g_fontSmall = nullptr;
    g_uiScale = scale;
    LoadHudFonts(scale);
    ApplyCasterTheme(scale);

    if (g_window != nullptr)
    {
        int minimumWidth = 0;
        int minimumHeight = 0;
        int maximumWidth = 0;
        int maximumHeight = 0;
        ScaledWindowLimits(
            scale,
            &minimumWidth,
            &minimumHeight,
            &maximumWidth,
            &maximumHeight);
        SDL_SetWindowMinimumSize(g_window, minimumWidth, minimumHeight);
        if (resizeWindow && previous > 0.0f)
        {
            int width = 0;
            int height = 0;
            if (SDL_GetWindowSize(g_window, &width, &height))
            {
                const float ratio = scale / previous;
                int targetWidth = std::max(
                    minimumWidth,
                    static_cast<int>(std::lround(static_cast<float>(width) * ratio)));
                int targetHeight = std::max(
                    minimumHeight,
                    static_cast<int>(std::lround(static_cast<float>(height) * ratio)));
                const float fit = std::min({
                    1.0f,
                    static_cast<float>(maximumWidth) / static_cast<float>(targetWidth),
                    static_cast<float>(maximumHeight) / static_cast<float>(targetHeight),
                });
                targetWidth = std::max(1, static_cast<int>(std::lround(targetWidth * fit)));
                targetHeight = std::max(1, static_cast<int>(std::lround(targetHeight * fit)));
                SDL_SetWindowSize(
                    g_window,
                    targetWidth,
                    targetHeight);
                CenterWindowOnCurrentDisplay(targetWidth, targetHeight);
                SessionLog::Step(
                    L"ui",
                    L"Scale changed requested=%.3f effective=%.3f previous=%.3f window=%dx%d",
                    requestedScale,
                    scale,
                    previous,
                    targetWidth,
                    targetHeight);
            }
        }
    }
}

void HudPill(const char* text, const ImVec4& color, bool filled = true)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float padX = Ui(9.0f);
    const float padY = Ui(3.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(textSize.x + padX * 2.0f, textSize.y + padY * 2.0f);
    const float rounding = 0.0f;
    const ImU32 fill = ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, filled ? 0.20f : 0.0f));
    const ImU32 border = ImGui::GetColorU32(color);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), fill, rounding);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border, rounding, 0, Ui(1.4f));
    dl->AddText(ImVec2(pos.x + padX, pos.y + padY), border, text);
    ImGui::Dummy(size);
}

bool HudPrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.118f, 0.608f, 0.745f, 0.41f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.72f, 0.82f, 0.58f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.412f, 0.922f, 0.922f, 0.66f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool ClickableText(const char* id, const char* text, const ImVec4& color)
{
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(sz.x, sz.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    const ImU32 c = ImGui::GetColorU32(hovered ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : color);
    ImGui::GetWindowDrawList()->AddText(pos, c, text);
    return clicked;
}

int HudSelector(const char* label, const char* const* options, int count, int current, float labelW = 132.0f)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kHudMuted, "%s", label);
    ImGui::SameLine(Ui(labelW));
    int result = current;
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine(0.0f, Ui(14.0f));
        }
        ImGui::PushID(i);
        if (i == current)
        {
            ImGui::TextColored(kAccent, "%s", options[i]);
        }
        else if (ClickableText("opt", options[i], kHudMuted))
        {
            result = i;
        }
        ImGui::PopID();
    }
    return result;
}

void HudTitle(const char* text)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = Ui(30.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(0, 0, 0, 225));
    draw->AddLine(pos, ImVec2(pos.x + width, pos.y), IM_COL32(255, 255, 255, 230), Ui(1.0f));
    draw->AddLine(ImVec2(pos.x, pos.y + height - Ui(1.0f)),
        ImVec2(pos.x + width, pos.y + height - Ui(1.0f)), IM_COL32(255, 255, 255, 230), Ui(1.0f));
    if (g_fontHeader != nullptr) { ImGui::PushFont(g_fontHeader); }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(pos.x + (width - textSize.x) * 0.5f,
        pos.y + (height - textSize.y) * 0.5f), IM_COL32_WHITE, text);
    if (g_fontHeader != nullptr) { ImGui::PopFont(); }
    ImGui::Dummy(ImVec2(width, height + Ui(4.0f)));
}

void HudRow(const char* label, const char* value, const ImVec4& valueColor, float labelW = 132.0f)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kHudMuted, "%s", label);
    ImGui::SameLine(Ui(labelW));
    ImGui::TextColored(valueColor, "%s", value);
}

void DrawPlayTab(CasterApp& app, const UiSnapshot& snap)
{
    auto& cfg = app.Config().Mutable();
    const bool transitionBusy = snap.phase == AppPhase::Hosting || snap.phase == AppPhase::Connecting
        || snap.phase == AppPhase::ActionRequired || snap.phase == AppPhase::Launching;
    const bool connected = snap.phase == AppPhase::Ready;
    const bool connectionLocked = transitionBusy || connected;
    constexpr float kLabelW = 96.0f;

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kHudMuted, "PUBLIC IP");
    ImGui::SameLine(Ui(kLabelW));
    {
        const std::string pub = ipinfo::PublicIp();
        if (ipinfo::PublicIpPending())
        {
            ImGui::TextColored(kHudWarn, "fetching\xE2\x80\xA6");
        }
        else if (pub.empty() || pub == "n/a")
        {
            ImGui::TextColored(kHudBad, "unavailable");
        }
        else
        {
            ImGui::TextUnformatted(pub.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("copy")) { ImGui::SetClipboardText(pub.c_str()); }
        }
    }

    ImGui::BeginDisabled(connectionLocked);
    static int stagedPort = -1;
    static std::uint16_t stagedPortSource = 0u;
    if (stagedPort < 0
        || (!ImGui::IsAnyItemActive() && stagedPortSource != cfg.port))
    {
        stagedPort = static_cast<int>(cfg.port);
        stagedPortSource = cfg.port;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kHudMuted, "PORT");
    ImGui::SameLine(Ui(kLabelW));
    ImGui::SetNextItemWidth(Ui(92.0f));
    ImGui::InputInt(
        "##connection_port",
        &stagedPort,
        0,
        0,
        ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        stagedPort = std::clamp(stagedPort, 1, 65535);
        cfg.port = static_cast<std::uint16_t>(stagedPort);
        stagedPortSource = cfg.port;
        SaveConfigOrWarn(app);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Host and default JOIN port");

    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kHudMuted, "JOIN");
    ImGui::SameLine(Ui(kLabelW));
    ImGui::SetNextItemWidth(Ui(190.0f));
    ImGui::InputTextWithHint(
        "##join_destination",
        "host or host:port",
        g_connectIp,
        sizeof(g_connectIp));
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        app.Config().SetLastConnectIp(std::wstring(g_connectIp, g_connectIp + std::strlen(g_connectIp)));
    }
    util::ConnectDestination parsedDestination{};
    const bool ipValid = g_connectIp[0] != '\0'
        && util::ParseConnectDestination(
            std::wstring(g_connectIp, g_connectIp + std::strlen(g_connectIp)),
            cfg.port,
            &parsedDestination);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ipValid);
    if (HudPrimaryButton("JOIN MATCH")) { ApplyConfigBuffers(cfg); app.StartConnect(std::wstring(g_connectIp, g_connectIp + std::strlen(g_connectIp))); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ipValid);
    if (ImGui::Button("WATCH MATCH")) { ApplyConfigBuffers(cfg); app.LaunchSpectator(std::wstring(g_connectIp, g_connectIp + std::strlen(g_connectIp))); }
    ImGui::EndDisabled();

    if (!cfg.recentConnectIps.empty())
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kHudMuted, "RECENT");
        ImGui::SameLine(Ui(kLabelW));
        const float recentColumnX = ImGui::GetCursorPosX();
        int idx = 0;
        float usedWidth = 0.0f;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        for (const std::wstring& destination : cfg.recentConnectIps)
        {
            const std::string display = WideToUtf8(destination);
            const float itemWidth = ImGui::CalcTextSize(display.c_str()).x;
            const float spacing = idx > 0 ? Ui(12.0f) : 0.0f;
            if (idx > 0 && usedWidth + spacing + itemWidth > availableWidth)
            {
                ImGui::SetCursorPosX(recentColumnX);
                usedWidth = 0.0f;
            }
            else if (idx > 0)
            {
                ImGui::SameLine(0.0f, spacing);
                usedWidth += spacing;
            }
            ImGui::PushID(idx++);
            if (ClickableText("recent_destination", display.c_str(), kAccent))
            {
                CopyToBuffer(g_connectIp, sizeof(g_connectIp), destination);
            }
            ImGui::PopID();
            usedWidth += itemWidth;
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::BeginDisabled(connectionLocked);
    if (HudPrimaryButton("HOST GAME", ImVec2(-1.0f, 0.0f))) { ApplyConfigBuffers(cfg); app.StartHost(); }
    ImGui::EndDisabled();
    if (snap.phase == AppPhase::Hosting || snap.phase == AppPhase::Connecting)
    {
        if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f))) { app.CancelNet(); }
    }

    if (connected)
    {
        ImGui::Separator();
        const ImVec4 latColor = snap.latencyMs < 30u ? kHudOk : (snap.latencyMs < 80u ? kHudWarn : kHudBad);
        HudPill("CONNECTED", kHudOk);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(latColor, "%u ms", snap.latencyMs);
        ImGui::SameLine(); ImGui::TextColored(kAccent, "%s", snap.localIsP1 ? "P1" : "P2");
        ImGui::SameLine(); ImGui::TextUnformatted(snap.isHost ? "Host" : "Guest");
        ImGui::SameLine();
        ImGui::TextColored(
            snap.connectionMethod == ConnectionMethod::AutoPunch ? kHudWarn : kHudOk,
            "%s",
            snap.connectionMethod == ConnectionMethod::AutoPunch ? "ASSISTED CONNECTION" : "DIRECT CONNECTION");
        ImGui::SameLine();
        const std::uint8_t displayedDelay = snap.isHost
            ? cfg.inputDelay : snap.readyInputDelay;
        ImGui::TextColored(kHudMuted, "Delay %u", static_cast<unsigned>(displayedDelay));
        ImGui::SameLine();
        const std::uint8_t displayedTargetFps = snap.isHost
            ? cfg.targetFps : snap.readyTargetFps;
        ImGui::TextColored(
            kHudMuted,
            "%u FPS",
            static_cast<unsigned>(displayedTargetFps));
        if (!snap.peerIp.empty()) { ImGui::SameLine(); ImGui::TextColored(kHudMuted, "%s", WideToUtf8(snap.peerIp).c_str()); }
        ImGui::Spacing();
        if (snap.isHost)
        {
            if (HudPrimaryButton("LAUNCH GAME", UiSize(-1.0f, 30.0f))) { app.LaunchReady(); }
        }
        else
        {
            ImGui::BeginDisabled(true);
            ImGui::Button("WAITING FOR HOST\xE2\x80\xA6", UiSize(-1.0f, 30.0f));
            ImGui::EndDisabled();
        }
    }

    ImGui::Separator();

    static bool s_wasConnected = false;
    static int s_recommendedDelay = 1;
    if (connected && !s_wasConnected)
    {
        const std::uint8_t recommendationTargetFps = snap.isHost
            ? cfg.targetFps
            : snap.readyTargetFps;
        s_recommendedDelay = timing::RecommendedInitialDelay(
            snap.latencyMs,
            static_cast<int>(snap.isHost ? cfg.inputDelay : snap.readyInputDelay),
            recommendationTargetFps);
        if (snap.isHost)
        {
            cfg.inputDelay = static_cast<std::uint8_t>(s_recommendedDelay);
            SaveConfigOrWarn(app);
        }
    }
    else if (!connected)
    {
        s_recommendedDelay = static_cast<int>(cfg.inputDelay);
    }
    s_wasConnected = connected;

    const int recommendedDelay = connected
        ? s_recommendedDelay
        : static_cast<int>(cfg.inputDelay);
    if (connected)
    {
        char pingText[32] = {};
        char recommendedText[48] = {};
        std::snprintf(pingText, sizeof(pingText), "%u ms", snap.latencyMs);
        if (snap.isHost)
        {
            std::snprintf(recommendedText, sizeof(recommendedText), "%d frames", recommendedDelay);
        }
        else
        {
            std::snprintf(
                recommendedText,
                sizeof(recommendedText),
                "%u frames",
                static_cast<unsigned>(snap.readyInputDelay));
        }
        HudRow("PING", pingText, snap.latencyMs < 80u ? kHudOk : kHudWarn, kLabelW);
        HudRow(snap.isHost ? "RECOMMENDED" : "HOST DELAY",
            recommendedText,
            kAccent,
            kLabelW);
    }
    else
    {
        ImGui::TextDisabled("A starting delay will be recommended after the connection is measured.");
    }

    const bool matchSettingsEditable = !transitionBusy && (!connected || snap.isHost);
    ImGui::BeginDisabled(!matchSettingsEditable);
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kHudMuted, "INITIAL DELAY");
        ImGui::SameLine(Ui(kLabelW));
        ImGui::SetNextItemWidth(Ui(220.0f));
        int delay = static_cast<int>(connected && !snap.isHost
            ? snap.readyInputDelay : cfg.inputDelay);
        if (ImGui::SliderInt("##delay", &delay, 0, 20, "%d frames"))
        {
            cfg.inputDelay = static_cast<std::uint8_t>(std::clamp(delay, 0, 20));
            SaveConfigOrWarn(app);
        }
        if (connected && snap.isHost)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Auto"))
            {
                cfg.inputDelay = static_cast<std::uint8_t>(recommendedDelay);
                SaveConfigOrWarn(app);
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kHudMuted, "GAME SPEED");
        ImGui::SameLine(Ui(kLabelW));
        ImGui::SetNextItemWidth(Ui(220.0f));
        const std::uint8_t displayedTargetFps = connected && !snap.isHost
            ? snap.readyTargetFps
            : cfg.targetFps;
        int targetFpsChoice = displayedTargetFps == proto::kMaximumTargetFps ? 1 : 0;
        if (ImGui::Combo("##target_fps", &targetFpsChoice, "60 FPS\0" "64 FPS\0"))
        {
            cfg.targetFps = targetFpsChoice == 1
                ? proto::kMaximumTargetFps
                : proto::kDefaultTargetFps;
            SaveConfigOrWarn(app);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "60 FPS prioritizes display compatibility. 64 FPS restores the game's faster "
                "original timing. The host's choice applies to both players and viewers.");
        }

        if (ImGui::CollapsingHeader("Advanced connection settings"))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kHudMuted, "MAX ROLLBACK");
            ImGui::SameLine(Ui(132.0f));
            ImGui::SetNextItemWidth(Ui(220.0f));
            int rollbackFrames = static_cast<int>(cfg.rollbackFrames);
            if (ImGui::SliderInt("##rollback_frames", &rollbackFrames, 0, 20, "%d frames"))
            {
                cfg.rollbackFrames = static_cast<std::uint8_t>(std::clamp(rollbackFrames, 0, 20));
                SaveConfigOrWarn(app);
            }
            ImGui::TextDisabled(
                "Limits how many late frames can be corrected. Every delay and limit combination is supported.");
        }
    }
    ImGui::EndDisabled();
    if (connected && !snap.isHost)
    {
        ImGui::TextDisabled(
            "The host chooses the starting delay, game speed, and maximum rollback for this match.");
    }

}

void DrawConnectionPrompt(CasterApp& app, const UiSnapshot& snap)
{
    const bool hostWaiting = snap.connectionPrompt == ConnectionPrompt::HostNotPlaying;
    const bool hostStarting = snap.connectionPrompt == ConnectionPrompt::HostStarting;
    const bool hostPlaying = snap.connectionPrompt == ConnectionPrompt::HostAlreadyPlaying;
    const char* const popupTitle = hostWaiting
        ? "MATCH NOT STARTED##connection_choice_waiting"
        : hostStarting
            ? "MATCH PREPARING##connection_choice_starting"
            : "MATCH ALREADY STARTED##connection_choice_active";
    if (hostWaiting || hostStarting || hostPlaying)
    {
        ImGui::OpenPopup(popupTitle);
    }
    ImGui::SetNextWindowSize(
        UiSize(hostWaiting ? 500.0f : 430.0f, 0.0f),
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            popupTitle,
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
    {
        return;
    }

    if (hostWaiting)
    {
        ImGui::TextWrapped(
            "The host is waiting for a player and there is no match to watch yet. You can wait for their next match or join as the second player.");
    }
    else if (hostStarting)
    {
        ImGui::TextWrapped(
            "Two players are already connected, but their game has not started yet. You can wait for the match viewer to open or go back.");
    }
    else
    {
        ImGui::TextWrapped(
            "The host is already playing. You cannot join this match as a player, but you can watch it without affecting either player.");
    }
    if (!snap.promptDestination.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(kHudMuted, "HOST  %s", WideToUtf8(snap.promptDestination).c_str());
    }
    ImGui::Spacing();
    if (hostWaiting)
    {
        if (HudPrimaryButton("WAIT FOR MATCH", UiSize(180.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
            app.WaitForSpectatorMatch();
        }
        ImGui::SameLine();
        if (ImGui::Button("JOIN AS PLAYER", UiSize(180.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
            app.JoinSpectatorHostAsPlayer();
        }
        ImGui::SameLine();
        if (ImGui::Button("BACK", UiSize(90.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
            app.DismissConnectionPrompt();
        }
    }
    else if (hostStarting)
    {
        if (HudPrimaryButton("WAIT TO WATCH", UiSize(205.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
            app.WaitForSpectatorMatch();
        }
        ImGui::SameLine();
        if (ImGui::Button("BACK", UiSize(205.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
            app.DismissConnectionPrompt();
        }
    }
    else
    {
        if (HudPrimaryButton("WATCH MATCH", UiSize(205.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
            app.WatchAlreadyStartedMatch();
        }
        ImGui::SameLine();
        if (ImGui::Button("BACK", UiSize(205.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
            app.DismissConnectionPrompt();
        }
    }
    ImGui::EndPopup();
}

std::string FriendlyKeyBinding(const std::wstring& binding);

void DrawPracticeTab(CasterApp& app, const UiSnapshot& snap)
{
    const auto& shortcuts = app.Config().Get().controls.shortcuts;
    ImGui::TextWrapped(
        "Practice tools are available offline and are automatically disabled during online matches.");

    ImGui::Spacing();
    if (ImGui::BeginTable("practice_hotkeys", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [](const std::string& key, const std::string& action)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(key.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(action.c_str());
        };

        for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
        {
            if (descriptor.group == trialnet::shortcuts::Group::Network)
            {
                continue;
            }
            row(FriendlyKeyBinding(shortcuts[descriptor.action]), WideToUtf8(descriptor.label));
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Change these keyboard shortcuts on the Controls page.");
    const bool sessionActive = snap.phase == AppPhase::Hosting || snap.phase == AppPhase::Connecting
        || snap.phase == AppPhase::ActionRequired || snap.phase == AppPhase::Ready
        || snap.phase == AppPhase::Launching;
    ImGui::BeginDisabled(sessionActive);
    if (HudPrimaryButton("START OFFLINE PRACTICE", UiSize(-1.0f, 34.0f)))
    {
        app.LaunchOffline();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Starts the game with offline practice tools enabled.");
}
void CloseLauncherController(LauncherController& controller)
{
    if (controller.ownsJoystick && controller.joystick != nullptr)
    {
        SDL_CloseJoystick(controller.joystick);
    }
    controller.joystick = nullptr;
    controller.ownsJoystick = false;
    if (controller.gamepad != nullptr)
    {
        SDL_CloseGamepad(controller.gamepad);
        controller.gamepad = nullptr;
    }
    controller.name[0] = '\0';
}

void CloseLauncherControllers()
{
    for (LauncherController& controller : g_launcherControllers)
    {
        CloseLauncherController(controller);
    }
    g_nextLauncherControllerScanMs = SDL_GetTicks() + 250u;
}

bool IsLauncherControllerOpen(const SDL_JoystickID id)
{
    for (const LauncherController& controller : g_launcherControllers)
    {
        if (controller.joystick != nullptr && SDL_GetJoystickID(controller.joystick) == id)
        {
            return true;
        }
    }
    return false;
}

LauncherController* FirstFreeLauncherController()
{
    for (LauncherController& controller : g_launcherControllers)
    {
        if (controller.joystick == nullptr)
        {
            return &controller;
        }
    }
    return nullptr;
}

void FinishOpeningController(LauncherController& controller)
{
    if (controller.joystick == nullptr)
    {
        return;
    }
    const char* const name = SDL_GetJoystickName(controller.joystick);
    std::snprintf(
        controller.name,
        sizeof(controller.name),
        "%s",
        name != nullptr && name[0] != '\0' ? name : "Controller");
}

void OpenLauncherControllers(const bool force = false)
{
    if (force)
    {
        CloseLauncherControllers();
    }
    if (FirstFreeLauncherController() == nullptr)
    {
        return;
    }

    const Uint64 now = SDL_GetTicks();
    if (!force && now < g_nextLauncherControllerScanMs)
    {
        return;
    }
    g_nextLauncherControllerScanMs = now + 1000u;

    int count = 0;
    SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
    if (ids == nullptr || count <= 0)
    {
        if (ids != nullptr)
        {
            SDL_free(ids);
        }
        return;
    }

    // Mapped first, so a raw HID cannot hide a controller later in enumeration order.
    for (int i = 0; i < count; ++i)
    {
        if (!SDL_IsGamepad(ids[i]) || IsLauncherControllerOpen(ids[i]))
        {
            continue;
        }
        LauncherController* const controller = FirstFreeLauncherController();
        if (controller == nullptr)
        {
            break;
        }
        controller->gamepad = SDL_OpenGamepad(ids[i]);
        if (controller->gamepad != nullptr)
        {
            controller->joystick = SDL_GetGamepadJoystick(controller->gamepad);
            controller->ownsJoystick = false;
        }
        if (controller->joystick == nullptr && controller->gamepad != nullptr)
        {
            SDL_CloseGamepad(controller->gamepad);
            controller->gamepad = nullptr;
        }
        // A mapped device can still fail its high-level open; fall back to its raw joystick view.
        if (controller->joystick == nullptr)
        {
            controller->joystick = SDL_OpenJoystick(ids[i]);
            controller->ownsJoystick = controller->joystick != nullptr;
        }
        if (controller->joystick == nullptr)
        {
            continue;
        }
        FinishOpeningController(*controller);
    }

    for (int i = 0; i < count; ++i)
    {
        if (SDL_IsGamepad(ids[i]) || IsLauncherControllerOpen(ids[i]))
        {
            continue;
        }
        LauncherController* const controller = FirstFreeLauncherController();
        if (controller == nullptr)
        {
            break;
        }
        controller->joystick = SDL_OpenJoystick(ids[i]);
        controller->ownsJoystick = controller->joystick != nullptr;
        if (controller->joystick != nullptr)
        {
            FinishOpeningController(*controller);
        }
    }

    SDL_free(ids);
}

LauncherController* LauncherControllerForPlayer(
    const ControlBindings& bindings,
    const int playerIndex)
{
    const int deviceIndex = ControllerDeviceIndexForPlayer(bindings, playerIndex);
    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(g_launcherControllers.size()))
    {
        return nullptr;
    }
    LauncherController& controller = g_launcherControllers[static_cast<std::size_t>(deviceIndex)];
    return controller.joystick != nullptr ? &controller : nullptr;
}

int OpenLauncherControllerCount()
{
    int count = 0;
    for (const LauncherController& controller : g_launcherControllers)
    {
        count += controller.joystick != nullptr ? 1 : 0;
    }
    return count;
}

std::wstring NormalizePadBindingName(const char* rawName)
{
    if (rawName == nullptr || rawName[0] == '\0')
    {
        return L"NONE";
    }

    return trialnet::input::CanonicalizePadToken(Utf8ToWide(rawName));
}

std::wstring ScancodeToBindingName(const SDL_Scancode scancode)
{
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        return L"UNKNOWN";
    }

    const char* name = SDL_GetScancodeName(scancode);
    if (name == nullptr || name[0] == '\0')
    {
        return L"UNKNOWN";
    }

    std::wstring wide = Utf8ToWide(name);
    for (wchar_t& ch : wide)
    {
        if (ch >= L'a' && ch <= L'z')
        {
            ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
    }
    return wide;
}

bool IsRuntimeKeyboardScancodeSupported(const SDL_Scancode scancode)
{
    if ((scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        || (scancode >= SDL_SCANCODE_0 && scancode <= SDL_SCANCODE_9)
        || (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F24)
        || (scancode >= SDL_SCANCODE_KP_0 && scancode <= SDL_SCANCODE_KP_9))
    {
        return true;
    }
    switch (scancode)
    {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_RETURN: case SDL_SCANCODE_BACKSPACE:
    case SDL_SCANCODE_TAB: case SDL_SCANCODE_SPACE: case SDL_SCANCODE_PAGEUP:
    case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_HOME: case SDL_SCANCODE_END:
    case SDL_SCANCODE_INSERT: case SDL_SCANCODE_DELETE: case SDL_SCANCODE_CAPSLOCK:
    case SDL_SCANCODE_SCROLLLOCK: case SDL_SCANCODE_NUMLOCKCLEAR: case SDL_SCANCODE_PRINTSCREEN:
    case SDL_SCANCODE_PAUSE: case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI:
    case SDL_SCANCODE_APPLICATION: case SDL_SCANCODE_COMMA: case SDL_SCANCODE_PERIOD:
    case SDL_SCANCODE_SLASH: case SDL_SCANCODE_SEMICOLON: case SDL_SCANCODE_MINUS:
    case SDL_SCANCODE_EQUALS: case SDL_SCANCODE_LEFTBRACKET: case SDL_SCANCODE_RIGHTBRACKET:
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSBACKSLASH:
    case SDL_SCANCODE_APOSTROPHE: case SDL_SCANCODE_GRAVE: case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_KP_PLUS: case SDL_SCANCODE_KP_MINUS: case SDL_SCANCODE_KP_MULTIPLY:
    case SDL_SCANCODE_KP_DIVIDE: case SDL_SCANCODE_KP_PERIOD: case SDL_SCANCODE_KP_COMMA:
        return true;
    default:
        return false;
    }
}

void CancelRebind()
{
    g_rebindPlayer = -1;
    g_rebindSlot = -1;
    g_rebindTarget = RebindTarget::None;
    g_rebindSubject = RebindSubject::None;
    g_rebindShortcut = trialnet::shortcuts::Action::Count;
    g_rebindStartedMs = 0;
    g_rebindAxisArmed = false;
}

bool IsAssignedKeyboardToken(const std::wstring& value)
{
    const std::wstring token = trialnet::shortcuts::NormalizeBindingToken(value);
    return !token.empty() && token != L"NONE" && token != L"OFF" && token != L"_";
}

std::wstring FindShortcutConflict(
    const ControlBindings& controls,
    const std::wstring& candidate,
    const trialnet::shortcuts::Action excludedAction)
{
    const std::wstring token = trialnet::shortcuts::NormalizeBindingToken(candidate);
    if (!IsAssignedKeyboardToken(token))
    {
        return {};
    }

    // One physical key must never drive a fighter and a shortcut together, offline included.
    const PlayerBindings* const players[] = {&controls.p1, &controls.p2};
    for (std::size_t playerIndex = 0; playerIndex < std::size(players); ++playerIndex)
    {
        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            if (trialnet::shortcuts::NormalizeBindingToken(
                    players[playerIndex]->keys[static_cast<std::size_t>(slot)]) == token)
            {
                return util::Format(
                    L"%s is already assigned to Player %u %s.",
                    token.c_str(),
                    static_cast<unsigned>(playerIndex + 1u),
                    kSlotLabels[slot]);
            }
        }
    }

    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        if (descriptor.action != excludedAction
            && trialnet::shortcuts::NormalizeBindingToken(controls.shortcuts[descriptor.action]) == token)
        {
            return util::Format(
                L"%s is already assigned to %s.",
                token.c_str(),
                descriptor.label);
        }
    }
    return {};
}

std::wstring FindFightBindingConflict(
    const ControlBindings& controls,
    const std::wstring& candidate,
    const int excludedPlayer,
    const int excludedSlot)
{
    const std::wstring token = trialnet::shortcuts::NormalizeBindingToken(candidate);
    if (!IsAssignedKeyboardToken(token))
    {
        return {};
    }
    const PlayerBindings* const players[] = {&controls.p1, &controls.p2};
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            if (playerIndex == excludedPlayer && slot == excludedSlot)
            {
                continue;
            }
            if (trialnet::shortcuts::NormalizeBindingToken(
                    players[playerIndex]->keys[static_cast<std::size_t>(slot)]) == token)
            {
                return util::Format(
                    L"%s is already assigned to Player %d %s.",
                    token.c_str(),
                    playerIndex + 1,
                    kSlotLabels[slot]);
            }
        }
    }
    for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
    {
        if (trialnet::shortcuts::NormalizeBindingToken(
                controls.shortcuts[descriptor.action]) == token)
        {
            return util::Format(
                L"%s is reserved for the %s shortcut. Rebind or clear that shortcut first.",
                token.c_str(),
                descriptor.label);
        }
    }
    return {};
}

std::wstring FindPadBindingConflict(
    const PlayerBindings& player,
    const std::wstring& candidate,
    const int excludedSlot)
{
    const std::wstring token = trialnet::input::CanonicalizePadToken(candidate);
    if (!IsAssignedKeyboardToken(token))
    {
        return {};
    }
    for (int slot = 0; slot < kFightSlotCount; ++slot)
    {
        if (slot == excludedSlot)
        {
            continue;
        }
        if (trialnet::input::CanonicalizePadToken(
                player.pad[static_cast<std::size_t>(slot)]) == token)
        {
            return util::Format(
                L"%s is already assigned to %s on this controller.",
                token.c_str(),
                kSlotLabels[slot]);
        }
    }
    return {};
}

std::vector<std::wstring> ExistingShortcutConflicts(const ControlBindings& controls)
{
    std::vector<std::wstring> conflicts;
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < trialnet::shortcuts::kDescriptors.size();
         ++descriptorIndex)
    {
        const trialnet::shortcuts::Descriptor& descriptor =
            trialnet::shortcuts::kDescriptors[descriptorIndex];
        const std::wstring token = trialnet::shortcuts::NormalizeBindingToken(
            controls.shortcuts[descriptor.action]);
        if (!IsAssignedKeyboardToken(token))
        {
            continue;
        }

        bool found = false;
        const PlayerBindings* const players[] = {&controls.p1, &controls.p2};
        for (std::size_t playerIndex = 0; playerIndex < std::size(players) && !found; ++playerIndex)
        {
            for (int slot = 0; slot < kFightSlotCount; ++slot)
            {
                if (trialnet::shortcuts::NormalizeBindingToken(
                        players[playerIndex]->keys[static_cast<std::size_t>(slot)]) == token)
                {
                    conflicts.push_back(util::Format(
                        L"%s conflicts with Player %u %s.",
                        descriptor.label,
                        static_cast<unsigned>(playerIndex + 1u),
                        kSlotLabels[slot]));
                    found = true;
                    break;
                }
            }
        }
        if (found)
        {
            continue;
        }

        for (std::size_t otherIndex = 0; otherIndex < descriptorIndex; ++otherIndex)
        {
            const trialnet::shortcuts::Descriptor& other =
                trialnet::shortcuts::kDescriptors[otherIndex];
            if (trialnet::shortcuts::NormalizeBindingToken(controls.shortcuts[other.action]) == token)
            {
                conflicts.push_back(util::Format(
                    L"%s and %s both use %s.",
                    other.label,
                    descriptor.label,
                    token.c_str()));
                break;
            }
        }
    }
    return conflicts;
}

bool IsEventFromLauncherController(
    const LauncherController* const controller,
    const SDL_JoystickID which)
{
    return controller != nullptr
        && controller->joystick != nullptr
        && which == SDL_GetJoystickID(controller->joystick);
}

std::wstring GamepadAxisBindingName(const SDL_GamepadAxis axis, const Sint16 value)
{
    switch (axis)
    {
    case SDL_GAMEPAD_AXIS_LEFTX: return value < 0 ? L"LEFT_STICK_LEFT" : L"LEFT_STICK_RIGHT";
    case SDL_GAMEPAD_AXIS_LEFTY: return value < 0 ? L"LEFT_STICK_UP" : L"LEFT_STICK_DOWN";
    case SDL_GAMEPAD_AXIS_RIGHTX: return value < 0 ? L"RIGHT_STICK_LEFT" : L"RIGHT_STICK_RIGHT";
    case SDL_GAMEPAD_AXIS_RIGHTY: return value < 0 ? L"RIGHT_STICK_UP" : L"RIGHT_STICK_DOWN";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return L"LEFT_TRIGGER";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return L"RIGHT_TRIGGER";
    default: return L"";
    }
}

bool ProcessControlRebind(SDL_Event& event, CasterApp& app)
{
    if (g_rebindSubject == RebindSubject::None || g_rebindTarget == RebindTarget::None)
    {
        return false;
    }

    if (!ConfigurationIsEditable(app.Snapshot().phase))
    {
        CancelRebind();
        return false;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
    {
        g_controlBindingWarning.clear();
        CancelRebind();
        return true;
    }

    auto& cfg = app.Config().Mutable();

    if (g_rebindSubject == RebindSubject::Shortcut)
    {
        if (g_rebindShortcut == trialnet::shortcuts::Action::Count
            || event.type != SDL_EVENT_KEY_DOWN)
        {
            return false;
        }

        const trialnet::shortcuts::Descriptor& descriptor =
            trialnet::shortcuts::Describe(g_rebindShortcut);
        const SDL_Scancode scancode = event.key.scancode;
        if (scancode == SDL_SCANCODE_DELETE || scancode == SDL_SCANCODE_BACKSPACE)
        {
            cfg.controls.shortcuts[g_rebindShortcut] = L"NONE";
            if (SaveConfigOrWarn(app))
            {
                app.AppendLog(util::Format(L"Cleared %s shortcut", descriptor.label));
                g_controlBindingWarning.clear();
            }
            CancelRebind();
            return true;
        }
        if (scancode == SDL_SCANCODE_UNKNOWN || !IsRuntimeKeyboardScancodeSupported(scancode))
        {
            g_controlBindingWarning =
                L"That key cannot be read reliably while the game has focus. Choose another key.";
            app.AppendLog(g_controlBindingWarning);
            return true;
        }

        const std::wstring binding = ScancodeToBindingName(scancode);
        g_controlBindingWarning = FindShortcutConflict(
            cfg.controls,
            binding,
            g_rebindShortcut);
        if (!g_controlBindingWarning.empty())
        {
            app.AppendLog(g_controlBindingWarning);
            return true;
        }

        cfg.controls.shortcuts[g_rebindShortcut] = binding;
        if (SaveConfigOrWarn(app))
        {
            app.AppendLog(util::Format(
                L"Bound %s to %s",
                descriptor.label,
                binding.c_str()));
        }
        CancelRebind();
        return true;
    }

    if (g_rebindSubject != RebindSubject::FightControl
        || g_rebindPlayer < 0
        || g_rebindSlot < 0
        || g_rebindSlot >= kFightSlotCount)
    {
        CancelRebind();
        return false;
    }

    PlayerBindings& player = g_rebindPlayer == 1 ? cfg.controls.p2 : cfg.controls.p1;
    const auto slot = static_cast<std::size_t>(g_rebindSlot);
    LauncherController* const controller =
        LauncherControllerForPlayer(cfg.controls, g_rebindPlayer);

    if (g_rebindTarget == RebindTarget::Keyboard)
    {
        if (event.type != SDL_EVENT_KEY_DOWN)
        {
            return false;
        }

        const SDL_Scancode scancode = event.key.scancode;
        if (scancode == SDL_SCANCODE_DELETE || scancode == SDL_SCANCODE_BACKSPACE)
        {
            player.keys[slot] = L"NONE";
            if (SaveConfigOrWarn(app))
            {
                app.AppendLog(util::Format(
                    L"Cleared P%d %s keyboard binding",
                    g_rebindPlayer + 1,
                    kSlotLabels[g_rebindSlot]));
                g_controlBindingWarning.clear();
            }
            CancelRebind();
            return true;
        }
        if (scancode == SDL_SCANCODE_UNKNOWN || !IsRuntimeKeyboardScancodeSupported(scancode))
        {
            g_controlBindingWarning =
                L"That key cannot be read reliably while the game has focus. Choose another key.";
            app.AppendLog(g_controlBindingWarning);
            return true;
        }

        const std::wstring binding = ScancodeToBindingName(scancode);
        g_controlBindingWarning = FindFightBindingConflict(
            cfg.controls,
            binding,
            g_rebindPlayer,
            g_rebindSlot);
        if (!g_controlBindingWarning.empty())
        {
            app.AppendLog(g_controlBindingWarning);
            return true;
        }

        player.keys[slot] = binding;
        if (SaveConfigOrWarn(app))
        {
            app.AppendLog(util::Format(
                L"Bound P%d %s key to %s",
                g_rebindPlayer + 1,
                kSlotLabels[g_rebindSlot],
                player.keys[slot].c_str()));
            g_controlBindingWarning.clear();
        }
        CancelRebind();
        return true;
    }

    if (g_rebindTarget == RebindTarget::Gamepad)
    {
        const Uint64 elapsed = SDL_GetTicks() - g_rebindStartedMs;
        const auto commitPadBinding = [&](const std::wstring& binding) {
            if (binding.empty())
            {
                return false;
            }
            g_controlBindingWarning = FindPadBindingConflict(
                player,
                binding,
                g_rebindSlot);
            if (!g_controlBindingWarning.empty())
            {
                app.AppendLog(g_controlBindingWarning);
                return false;
            }
            player.pad[slot] = binding;
            if (SaveConfigOrWarn(app))
            {
                app.AppendLog(util::Format(
                    L"Bound P%d %s controller to %s",
                    g_rebindPlayer + 1,
                    kSlotLabels[g_rebindSlot],
                    player.pad[slot].c_str()));
            }
            CancelRebind();
            return true;
        };
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
            && controller != nullptr
            && controller->gamepad != nullptr
            && IsEventFromLauncherController(controller, event.gbutton.which))
        {
            if (elapsed < 100u)
            {
                return true;
            }
            const char* const name = SDL_GetGamepadStringForButton(
                static_cast<SDL_GamepadButton>(event.gbutton.button));
            commitPadBinding(NormalizePadBindingName(name));
            return true;
        }

        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
            && controller != nullptr
            && controller->gamepad != nullptr
            && IsEventFromLauncherController(controller, event.gaxis.which))
        {
            if (std::abs(static_cast<int>(event.gaxis.value)) <= 8000 && elapsed >= 100u)
            {
                g_rebindAxisArmed = true;
                return true;
            }
            if (!g_rebindAxisArmed || std::abs(static_cast<int>(event.gaxis.value)) < 16000)
            {
                return true;
            }
            commitPadBinding(GamepadAxisBindingName(
                static_cast<SDL_GamepadAxis>(event.gaxis.axis), event.gaxis.value));
            return true;
        }

        // Mapped pads emit duplicate JOYSTICK events; raw is authoritative only if unmapped.
        if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN
            && controller != nullptr
            && controller->gamepad == nullptr
            && IsEventFromLauncherController(controller, event.jbutton.which))
        {
            if (elapsed < 100u)
            {
                return true;
            }
            commitPadBinding(util::Format(
                L"JOY_BUTTON_%u",
                static_cast<unsigned>(event.jbutton.button)));
            return true;
        }

        if (event.type == SDL_EVENT_JOYSTICK_AXIS_MOTION
            && controller != nullptr
            && controller->gamepad == nullptr
            && IsEventFromLauncherController(controller, event.jaxis.which))
        {
            if (std::abs(static_cast<int>(event.jaxis.value)) <= 8000 && elapsed >= 100u)
            {
                g_rebindAxisArmed = true;
                return true;
            }
            if (!g_rebindAxisArmed || std::abs(static_cast<int>(event.jaxis.value)) < 16000)
            {
                return true;
            }
            commitPadBinding(util::Format(
                L"JOY_AXIS_%u_%s",
                static_cast<unsigned>(event.jaxis.axis),
                event.jaxis.value < 0 ? L"NEG" : L"POS"));
            return true;
        }

        if (event.type == SDL_EVENT_JOYSTICK_HAT_MOTION
            && controller != nullptr
            && controller->gamepad == nullptr
            && IsEventFromLauncherController(controller, event.jhat.which)
            && event.jhat.value != SDL_HAT_CENTERED)
        {
            if (elapsed < 100u)
            {
                return true;
            }
            const wchar_t* direction = (event.jhat.value & SDL_HAT_UP) != 0 ? L"UP"
                : (event.jhat.value & SDL_HAT_DOWN) != 0 ? L"DOWN"
                : (event.jhat.value & SDL_HAT_LEFT) != 0 ? L"LEFT" : L"RIGHT";
            commitPadBinding(util::Format(
                L"JOY_HAT_%u_%s",
                static_cast<unsigned>(event.jhat.hat),
                direction));
            return true;
        }

        if (event.type == SDL_EVENT_KEY_DOWN)
        {
            if (event.key.scancode == SDL_SCANCODE_DELETE || event.key.scancode == SDL_SCANCODE_BACKSPACE)
            {
                player.pad[slot] = L"NONE";
                if (SaveConfigOrWarn(app))
                {
                    app.AppendLog(util::Format(
                        L"Cleared P%d %s pad binding",
                        g_rebindPlayer + 1,
                        kSlotLabels[g_rebindSlot]));
                }
                CancelRebind();
                return true;
            }
        }
    }

    return false;
}

std::string TitleCaseBinding(std::string text)
{
    bool wordStart = true;
    for (char& ch : text)
    {
        if (ch == '_')
        {
            ch = ' ';
            wordStart = true;
        }
        else if (ch >= 'A' && ch <= 'Z' && !wordStart)
        {
            ch = static_cast<char>(ch - 'A' + 'a');
            wordStart = false;
        }
        else
        {
            wordStart = ch == ' ';
        }
    }
    return text;
}

std::string FriendlyKeyBinding(const std::wstring& binding)
{
    const std::wstring token = trialnet::input::NormalizeAsciiToken(binding);
    if (token.empty() || token == L"NONE" || token == L"OFF" || token == L"-")
    {
        return "Not assigned";
    }
    std::string text = WideToUtf8(binding);
    if (text.rfind("KP_", 0) == 0)
    {
        text = "NUMPAD " + text.substr(3);
    }
    if (text == "UP" || text == "DOWN" || text == "LEFT" || text == "RIGHT")
    {
        text += " ARROW";
    }
    return TitleCaseBinding(text.empty() ? std::string("Not assigned") : text);
}

std::string FriendlyPadBinding(const std::wstring& binding)
{
    using Button = trialnet::input::StandardButton;
    switch (trialnet::input::ParseStandardButton(binding))
    {
    case Button::South: return "A / Cross";
    case Button::East: return "B / Circle";
    case Button::West: return "X / Square";
    case Button::North: return "Y / Triangle";
    case Button::Back: return "View / Share";
    case Button::Guide: return "Home";
    case Button::Start: return "Menu / Options";
    case Button::LeftStick: return "Left Stick Click";
    case Button::RightStick: return "Right Stick Click";
    case Button::LeftShoulder: return "Left Shoulder (L1 / LB)";
    case Button::RightShoulder: return "Right Shoulder (R1 / RB)";
    case Button::DpadUp: return "D-Pad Up";
    case Button::DpadDown: return "D-Pad Down";
    case Button::DpadLeft: return "D-Pad Left";
    case Button::DpadRight: return "D-Pad Right";
    case Button::Touchpad: return "Touchpad Button";
    case Button::Invalid: break;
    default: return "Extra Controller Button";
    }

    const std::wstring token = trialnet::input::NormalizeAsciiToken(binding);
    if (token.empty() || token == L"NONE" || token == L"OFF" || token == L"-")
    {
        return "Not assigned";
    }
    if (token == L"LEFT_STICK_UP") return "Left Stick Up";
    if (token == L"LEFT_STICK_DOWN") return "Left Stick Down";
    if (token == L"LEFT_STICK_LEFT") return "Left Stick Left";
    if (token == L"LEFT_STICK_RIGHT") return "Left Stick Right";
    if (token == L"RIGHT_STICK_UP") return "Right Stick Up";
    if (token == L"RIGHT_STICK_DOWN") return "Right Stick Down";
    if (token == L"RIGHT_STICK_LEFT") return "Right Stick Left";
    if (token == L"RIGHT_STICK_RIGHT") return "Right Stick Right";
    if (token == L"LEFT_TRIGGER") return "Left Trigger (L2 / LT)";
    if (token == L"RIGHT_TRIGGER") return "Right Trigger (R2 / RT)";

    int index = -1;
    wchar_t direction[16] = {};
    if (swscanf_s(token.c_str(), L"JOY_BUTTON_%d", &index) == 1 && index >= 0)
    {
        return "Controller Button " + std::to_string(index + 1);
    }
    if (swscanf_s(token.c_str(), L"JOY_AXIS_%d_%15ls", &index, direction,
            static_cast<unsigned>(std::size(direction))) == 2 && index >= 0)
    {
        return "Controller Axis " + std::to_string(index + 1)
            + (std::wcscmp(direction, L"NEG") == 0 ? " -" : " +");
    }
    if (swscanf_s(token.c_str(), L"JOY_HAT_%d_%15ls", &index, direction,
            static_cast<unsigned>(std::size(direction))) == 2)
    {
        return "D-Pad " + TitleCaseBinding(WideToUtf8(direction));
    }
    return binding.empty() ? "Not assigned" : TitleCaseBinding(WideToUtf8(binding));
}

std::string FriendlyBinding(const PlayerBindings& bindings, const int slot)
{
    const auto index = static_cast<std::size_t>(slot);
    return bindings.source == trialnet::input::Source::Controller
        ? FriendlyPadBinding(bindings.pad[index])
        : FriendlyKeyBinding(bindings.keys[index]);
}

bool LauncherControllerAxesAreNeutral(const LauncherController* const controller)
{
    if (controller == nullptr)
    {
        return false;
    }
    if (controller->gamepad != nullptr)
    {
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
        {
            if (std::abs(static_cast<int>(SDL_GetGamepadAxis(
                    controller->gamepad,
                    static_cast<SDL_GamepadAxis>(axis)))) > 8000)
            {
                return false;
            }
        }
        return true;
    }

    if (controller->joystick != nullptr)
    {
        const int axes = SDL_GetNumJoystickAxes(controller->joystick);
        for (int axis = 0; axis < axes; ++axis)
        {
            if (std::abs(static_cast<int>(SDL_GetJoystickAxis(controller->joystick, axis))) > 8000)
            {
                return false;
            }
        }
    }
    return true;
}

void BeginRebind(
    const int playerIndex,
    const int slot,
    const trialnet::input::Source source,
    const LauncherController* const controller)
{
    g_rebindPlayer = playerIndex;
    g_rebindSlot = slot;
    g_rebindSubject = RebindSubject::FightControl;
    g_rebindShortcut = trialnet::shortcuts::Action::Count;
    g_rebindTarget = source == trialnet::input::Source::Controller
        ? RebindTarget::Gamepad : RebindTarget::Keyboard;
    g_controlBindingWarning.clear();
    g_rebindStartedMs = SDL_GetTicks();
    // A held stick must return to neutral first, or click-through noise becomes the binding.
    g_rebindAxisArmed = g_rebindTarget == RebindTarget::Gamepad
        && LauncherControllerAxesAreNeutral(controller);
}

void BeginShortcutRebind(const trialnet::shortcuts::Action action)
{
    g_rebindPlayer = -1;
    g_rebindSlot = -1;
    g_rebindSubject = RebindSubject::Shortcut;
    g_rebindShortcut = action;
    g_rebindTarget = RebindTarget::Keyboard;
    g_controlBindingWarning.clear();
    g_rebindStartedMs = SDL_GetTicks();
    g_rebindAxisArmed = false;
}

void DrawPlayerBindings(
    CasterApp& app,
    const wchar_t* title,
    const int playerIndex,
    PlayerBindings& bindings,
    LauncherController* const controller)
{
    ImGui::PushID(playerIndex);
    ImGui::SeparatorText(WideToUtf8(title).c_str());

    const char* sourceOptions[] = {"Keyboard", "Controller"};
    const int currentSource = bindings.source == trialnet::input::Source::Controller ? 1 : 0;
    const int selectedSource = HudSelector("INPUT DEVICE", sourceOptions, 2, currentSource, 112.0f);
    if (selectedSource != currentSource)
    {
        bindings.source = selectedSource == 1
            ? trialnet::input::Source::Controller : trialnet::input::Source::Keyboard;
        CancelRebind();
        if (SaveConfigOrWarn(app))
        {
            app.AppendLog(util::Format(
                L"P%d fight controls now use %s only",
                playerIndex + 1,
                trialnet::input::SourceIniName(bindings.source)));
        }
    }

    if (bindings.source == trialnet::input::Source::Controller && controller == nullptr)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kHudMuted);
        ImGui::TextWrapped("Connect a controller; it will appear automatically.");
        ImGui::PopStyleColor();
    }
    else if (bindings.source == trialnet::input::Source::Controller)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kHudMuted);
        ImGui::TextWrapped("Controller: %s", controller->name);
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginTable(
            "binding_rows",
            3,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_PadOuterX))
    {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, Ui(48.0f));
        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed, Ui(43.0f));

        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            ImGui::PushID(slot);
            const bool waiting = g_rebindPlayer == playerIndex && g_rebindSlot == slot;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", WideToUtf8(kSlotLabels[slot]).c_str());

            ImGui::TableSetColumnIndex(1);
            const std::string shown = waiting
                ? (bindings.source == trialnet::input::Source::Controller
                    ? "Press a control" : "Press a key")
                : FriendlyBinding(bindings, slot);
            if (ImGui::Button((shown + "###binding").c_str(), ImVec2(-1.0f, 0.0f)))
            {
                BeginRebind(playerIndex, slot, bindings.source, controller);
            }

            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("Clear###clear"))
            {
                auto& selected = bindings.source == trialnet::input::Source::Controller
                    ? bindings.pad : bindings.keys;
                selected[static_cast<std::size_t>(slot)] = L"NONE";
                g_controlBindingWarning.clear();
                CancelRebind();
                if (SaveConfigOrWarn(app))
                {
                    app.AppendLog(util::Format(
                        L"Cleared P%d %s %s binding",
                        playerIndex + 1,
                        kSlotLabels[slot],
                        trialnet::input::SourceIniName(bindings.source)));
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void DrawInputVerifier(
    const PlayerBindings& bindings,
    const int playerIndex,
    const char* playerLabel,
    const LauncherController* const controller)
{
    SDL_PumpEvents();
    ImGui::PushID(playerIndex);
    ImGui::SeparatorText(playerLabel);
    ImGui::TextDisabled("Press each control to confirm the game sees it.");

    if (ImGui::BeginTable("input_verify", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Ui(70.0f));
        ImGui::TableHeadersRow();

        for (int slot = 0; slot < kFightSlotCount; ++slot)
        {
            const bool down = IsSelectedSlotDown(
                bindings,
                slot,
                controller != nullptr ? controller->gamepad : nullptr,
                controller != nullptr ? controller->joystick : nullptr);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", WideToUtf8(kSlotLabels[slot]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FriendlyBinding(bindings, slot).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(down ? kHudOk : kHudMuted, down ? "Pressed" : "Ready");
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void DrawShortcutBindings(
    CasterApp& app,
    trialnet::shortcuts::Bindings& bindings,
    const trialnet::shortcuts::Group group,
    const char* const title,
    const char* const description)
{
    ImGui::PushID(static_cast<int>(group));
    ImGui::SeparatorText(title);
    ImGui::TextDisabled("%s", description);

    if (ImGui::BeginTable(
            "shortcut_rows",
            3,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_PadOuterX))
    {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Keyboard shortcut", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed, Ui(43.0f));

        for (const trialnet::shortcuts::Descriptor& descriptor : trialnet::shortcuts::kDescriptors)
        {
            if (descriptor.group != group)
            {
                continue;
            }

            ImGui::PushID(static_cast<int>(descriptor.action));
            const bool waiting = g_rebindSubject == RebindSubject::Shortcut
                && g_rebindShortcut == descriptor.action;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(WideToUtf8(descriptor.label).c_str());

            ImGui::TableSetColumnIndex(1);
            const std::string shown = waiting
                ? "Press a key"
                : FriendlyKeyBinding(bindings[descriptor.action]);
            if (ImGui::Button((shown + "###shortcut_binding").c_str(), ImVec2(-1.0f, 0.0f)))
            {
                BeginShortcutRebind(descriptor.action);
            }

            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("Clear###clear_shortcut"))
            {
                bindings[descriptor.action] = L"NONE";
                g_controlBindingWarning.clear();
                CancelRebind();
                if (SaveConfigOrWarn(app))
                {
                    app.AppendLog(util::Format(L"Cleared %s shortcut", descriptor.label));
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void DrawControlsTab(CasterApp& app, const UiSnapshot& snap)
{
    auto& cfg = app.Config().Mutable();
    const bool editable = ConfigurationIsEditable(snap.phase);

    if (!editable && g_rebindSubject != RebindSubject::None)
    {
        CancelRebind();
    }

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float contentWidth = std::min(Ui(920.0f), availableWidth);
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() + std::max(0.0f, (availableWidth - contentWidth) * 0.5f));
    ImGui::BeginChild("controls_centered", ImVec2(contentWidth, 0.0f), false);

    ImGui::TextWrapped(
        "Choose one device for each local player, then select an action and press the key or "
        "button you want. Only the selected device controls that player.");
    ImGui::PushStyleColor(ImGuiCol_Text, kHudMuted);
    ImGui::TextWrapped(
        "Escape cancels setup and is reserved for leaving a game. Keyboard shortcuts can be "
        "rebound below; Backspace or Delete clears the selected binding.");
    ImGui::PopStyleColor();

    if (!editable)
    {
        ImGui::TextColored(
            kHudWarn,
            "Controls are locked while a connection or game is active.");
    }

    const int controllerCount = OpenLauncherControllerCount();
    ImGui::TextDisabled(
        "Controllers found: %d%s",
        controllerCount,
        controllerCount == static_cast<int>(g_launcherControllers.size()) ? "" : " (updates automatically)");

    if (g_rebindSubject == RebindSubject::FightControl && g_rebindPlayer >= 0)
    {
        ImGui::TextColored(
            ImVec4(0.4f, 0.85f, 1.0f, 1.0f),
            "Waiting for P%d %s (%s)",
            g_rebindPlayer + 1,
            WideToUtf8(kSlotLabels[g_rebindSlot]).c_str(),
            g_rebindTarget == RebindTarget::Gamepad ? "controller" : "keyboard");
    }
    else if (g_rebindSubject == RebindSubject::Shortcut
        && g_rebindShortcut != trialnet::shortcuts::Action::Count)
    {
        ImGui::TextColored(
            kAccent,
            "Waiting for %s (keyboard)",
            WideToUtf8(trialnet::shortcuts::Describe(g_rebindShortcut).label).c_str());
    }

    if (!g_controlBindingWarning.empty())
    {
        ImGui::TextColored(kHudBad, "%s", WideToUtf8(g_controlBindingWarning).c_str());
    }
    const std::vector<std::wstring> existingConflicts = ExistingShortcutConflicts(cfg.controls);
    for (const std::wstring& conflict : existingConflicts)
    {
        ImGui::TextColored(kHudBad, "%s", WideToUtf8(conflict).c_str());
    }

    ImGui::BeginDisabled(!editable);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, UiSize(10.0f, 3.0f));
    if (ImGui::BeginTable(
            "player_binding_columns",
            2,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_PadOuterX))
    {
        ImGui::TableSetupColumn("Player 1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Player 2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawPlayerBindings(
            app,
            L"Player 1 (online and offline)",
            0,
            cfg.controls.p1,
            LauncherControllerForPlayer(cfg.controls, 0));
        ImGui::TableSetColumnIndex(1);
        DrawPlayerBindings(
            app,
            L"Player 2 (offline only)",
            1,
            cfg.controls.p2,
            LauncherControllerForPlayer(cfg.controls, 1));
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    const float resetWidth = Ui(180.0f);
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - resetWidth) * 0.5f));
    if (ImGui::Button("Reset both players", ImVec2(resetWidth, 0.0f)))
    {
        const ControlBindings defaults = DefaultControlBindings();
        ControlBindings candidate = cfg.controls;
        candidate.p1 = defaults.p1;
        candidate.p2 = defaults.p2;
        if (!ValidateControlBindings(candidate, &g_controlBindingWarning))
        {
            app.AppendLog(g_controlBindingWarning);
        }
        else
        {
            cfg.controls = std::move(candidate);
            g_controlBindingWarning.clear();
            if (SaveConfigOrWarn(app))
            {
                app.AppendLog(L"Player controls reset to defaults");
            }
        }
        CancelRebind();
    }

    ImGui::Spacing();
    DrawShortcutBindings(
        app,
        cfg.controls.shortcuts,
        trialnet::shortcuts::Group::Network,
        "Online delay controls",
        "Change your own input delay during an online battle. These shortcuts remain available "
        "whether online practice shortcuts are enabled or not.");
    DrawShortcutBindings(
        app,
        cfg.controls.shortcuts,
        trialnet::shortcuts::Group::TrialPractice,
        "Trial practice shortcuts",
        "Trial's safe training and resource actions (F4 and F5 by default). Online, they work "
        "only when both players enable practice shortcuts.");
    DrawShortcutBindings(
        app,
        cfg.controls.shortcuts,
        trialnet::shortcuts::Group::ModPractice,
        "Offline practice tools",
        "These launcher practice tools are available offline only and remain disabled during "
        "online play.");

    const float shortcutResetWidth = Ui(180.0f);
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX()
        + std::max(0.0f, (ImGui::GetContentRegionAvail().x - shortcutResetWidth) * 0.5f));
    if (ImGui::Button("Reset shortcuts", ImVec2(shortcutResetWidth, 0.0f)))
    {
        ControlBindings candidate = cfg.controls;
        candidate.shortcuts = trialnet::shortcuts::DefaultBindings();
        CancelRebind();
        if (!ValidateControlBindings(candidate, &g_controlBindingWarning))
        {
            app.AppendLog(g_controlBindingWarning);
        }
        else
        {
            cfg.controls = std::move(candidate);
            g_controlBindingWarning.clear();
            if (SaveConfigOrWarn(app))
            {
                app.AppendLog(L"Keyboard shortcuts reset to defaults");
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Test controls"))
    {
        if (ImGui::BeginTable(
                "player_verifier_columns",
                2,
                ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawInputVerifier(
                cfg.controls.p1,
                0,
                "Player 1",
                LauncherControllerForPlayer(cfg.controls, 0));
            ImGui::TableSetColumnIndex(1);
            DrawInputVerifier(
                cfg.controls.p2,
                1,
                "Player 2",
                LauncherControllerForPlayer(cfg.controls, 1));
            ImGui::EndTable();
        }
    }

    ImGui::EndDisabled();

    ImGui::EndChild();
}

void DrawSettingsTab(CasterApp& app, const UiSnapshot& snap)
{
    auto& cfg = app.Config().Mutable();
    static int stagedInterfaceScale = -1;
    const bool editable = ConfigurationIsEditable(snap.phase);

    if (!editable)
    {
        ImGui::TextColored(
            kHudWarn,
            "Settings are locked while a connection or game is active.");
    }

    ImGui::BeginDisabled(!editable);

    ImGui::SeparatorText("Profile");
    ImGui::SetNextItemWidth(Ui(260.0f));
    ImGui::InputText("Player name", g_playerName, sizeof(g_playerName));

    ImGui::SeparatorText("Installation");
    ImGui::TextColored(kHudOk, "Game files found");
    ImGui::TextDisabled("Keep the launcher in your game folder.");

    ImGui::TextColored(kHudOk, "Online play ready");
    ImGui::TextDisabled(
        "Your side and controls are assigned automatically when a match starts.");

    ImGui::SeparatorText("Online matches");
    if (ImGui::Checkbox("Allow practice shortcuts online", &cfg.allowPracticeKeys))
    {
        if (SaveConfigOrWarn(app))
        {
            app.AppendLog(cfg.allowPracticeKeys
                ? L"Online practice shortcuts enabled for future matches"
                : L"Online practice shortcuts disabled");
        }
    }
    ImGui::TextDisabled(
        "This takes effect only when both players enable it. Trial's synchronized training "
        "and resource shortcuts (F4 and F5 by default) can then be used online.");
    ImGui::TextDisabled(
        "Save states, frame stepping, player switching, and visual practice tools remain "
        "offline-only because they would interrupt an online match.");

    ImGui::SeparatorText("Connection");
    ImGui::TextDisabled(
        "A direct connection is tried first. Connection assistance is used automatically when needed.");

    ImGui::Checkbox("Prioritize game performance", &cfg.highPriority);
    ImGui::Checkbox("Verify game files before launch", &cfg.verifyExeHash);

    ImGui::SeparatorText("Appearance");
    if (stagedInterfaceScale < 0)
    {
        stagedInterfaceScale = static_cast<int>(cfg.uiScalePercent);
    }
    ImGui::SetNextItemWidth(Ui(240.0f));
    ImGui::SliderInt("Interface size", &stagedInterfaceScale, 75, 200, "%d%%");
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        cfg.uiScalePercent = static_cast<std::uint16_t>(
            std::clamp(stagedInterfaceScale, 75, 200));
        SaveConfigOrWarn(app);
    }
    ImGui::TextDisabled(
        "Display scaling is automatic. Your preferred size is applied when you release the slider.");

    ImGui::EndDisabled();

    if (ImGui::CollapsingHeader("Diagnostics"))
    {
        ImGui::Text("Launcher version: %s", WideToUtf8(snap.launcherVersion).c_str());
        ImGui::Text("Netplay DLL version: %s", WideToUtf8(snap.netplayDllVersion).c_str());
        ImGui::Text("Runtime: %s", WideToUtf8(snap.compatibilityRuntime).c_str());

        ImGui::BeginDisabled(!editable);
        if (ImGui::Checkbox("Save diagnostic logs", &cfg.diagnosticLogging))
        {
            if (!SaveConfigOrWarn(app))
            {
                cfg.diagnosticLogging = !cfg.diagnosticLogging;
            }
            else
            {
                app.AppendLog(cfg.diagnosticLogging
                    ? L"Diagnostic logging will start after the launcher is restarted"
                    : L"Diagnostic logging will stop after the launcher is restarted");
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "Off by default. Enable this only when collecting troubleshooting information.");

        const bool loggingActive = SessionLog::IsFileLoggingEnabled();
        if (cfg.diagnosticLogging != loggingActive)
        {
            ImGui::TextColored(kHudWarn, "Restart the launcher to apply this change.");
        }
        if (loggingActive)
        {
            ImGui::TextDisabled(
                "Session log: %s",
                WideToUtf8(SessionLog::LogFilePath().wstring()).c_str());
            ImGui::TextDisabled(
                "Launch manifest: %s",
                WideToUtf8((SessionLog::LogDirectory() / L"launches.tsv").wstring()).c_str());
        }
        else
        {
            ImGui::TextDisabled("No diagnostic files are being written this launch.");
        }
    }

    ImGui::BeginDisabled(!editable);
    if (ImGui::Button("Save settings"))
    {
        cfg.uiScalePercent = static_cast<std::uint16_t>(
            std::clamp(stagedInterfaceScale, 75, 200));
        ApplyConfigBuffers(cfg);
        if (SaveConfigOrWarn(app))
        {
            app.AppendLog(L"Settings saved");
        }
    }
    ImGui::EndDisabled();
}

void PhaseInfo(AppPhase phase, const char*& label, ImVec4& color)
{
    switch (phase)
    {
    case AppPhase::Idle: label = "OFFLINE"; color = kHudMuted; break;
    case AppPhase::Hosting: label = "HOSTING"; color = kHudWarn; break;
    case AppPhase::Connecting: label = "CONNECTING"; color = kHudWarn; break;
    case AppPhase::ActionRequired: label = "CHOOSE"; color = kHudWarn; break;
    case AppPhase::Ready: label = "READY"; color = kHudOk; break;
    case AppPhase::Launching: label = "LAUNCHING"; color = kAccent; break;
    case AppPhase::Error: label = "ERROR"; color = kHudBad; break;
    default: label = "\xE2\x80\x94"; color = kHudMuted; break;
    }
}

void HudHeader(const UiSnapshot& snap)
{
    (void)snap;
    const float topBarH = Ui(32.0f);
    const float artworkH = Ui(72.0f);
    const float headerH = topBarH + artworkH;
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float ww = ImGui::GetWindowSize().x;
    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = wpos;
    const ImVec2 topBarEnd(wpos.x + ww, wpos.y + topBarH);
    const ImVec2 artworkEnd(wpos.x + ww, wpos.y + headerH);

    dl->AddRectFilled(p0, topBarEnd, IM_COL32(0, 0, 0, 255));
    dl->AddLine(p0, ImVec2(topBarEnd.x, p0.y), IM_COL32(255, 255, 255, 230), Ui(1.0f));
    dl->AddLine(ImVec2(p0.x, topBarEnd.y - Ui(1.0f)),
        ImVec2(topBarEnd.x, topBarEnd.y - Ui(1.0f)),
        IM_COL32(255, 255, 255, 230), Ui(1.0f));

    if (g_bannerTexture != nullptr)
    {
        float textureWidth = 0.0f;
        float textureHeight = 0.0f;
        const ImTextureID textureId =
            static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(g_bannerTexture));
        dl->AddRectFilled(ImVec2(p0.x, topBarEnd.y), artworkEnd, IM_COL32(0, 8, 26, 255));
        if (SDL_GetTextureSize(g_bannerTexture, &textureWidth, &textureHeight)
            && textureWidth > 0.0f && textureHeight > 0.0f)
        {
            const float targetWidth = std::max(1.0f, artworkEnd.x - p0.x);
            const float targetHeight = std::max(1.0f, artworkEnd.y - topBarEnd.y);
            const float sourceAspect = textureWidth / textureHeight;
            const float targetAspect = targetWidth / targetHeight;
            ImVec2 uv0(0.0f, 0.0f);
            ImVec2 uv1(1.0f, 1.0f);
            if (sourceAspect > targetAspect)
            {
                const float visibleWidth = targetAspect / sourceAspect;
                uv0.x = (1.0f - visibleWidth) * 0.5f;
                uv1.x = uv0.x + visibleWidth;
            }
            else if (sourceAspect < targetAspect)
            {
                const float visibleHeight = sourceAspect / targetAspect;
                uv0.y = (1.0f - visibleHeight) * 0.5f;
                uv1.y = uv0.y + visibleHeight;
            }
            dl->AddImage(
                textureId,
                ImVec2(p0.x, topBarEnd.y),
                artworkEnd,
                uv0,
                uv1);
        }
    }
    else
    {
        dl->AddRectFilled(ImVec2(p0.x, topBarEnd.y), artworkEnd, IM_COL32(0, 10, 30, 255));
    }
    dl->AddLine(
        ImVec2(p0.x, artworkEnd.y - Ui(1.0f)),
        ImVec2(artworkEnd.x, artworkEnd.y - Ui(1.0f)),
        IM_COL32(255, 255, 255, 210), Ui(1.0f));

    ImFont* hf = g_fontHeader != nullptr ? g_fontHeader : ImGui::GetFont();
    const char* title = "EFZ FF NETPLAY";
    ImGui::PushFont(hf);
    const ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::PopFont();
    dl->AddText(hf, hf->FontSize,
        ImVec2(p0.x + (ww - ts.x) * 0.5f, p0.y + (topBarH - ts.y) * 0.5f),
        IM_COL32_WHITE, title);

    ImGui::SetCursorScreenPos(ImVec2(p0.x + st.WindowPadding.x, p0.y + headerH + Ui(4.0f)));
}

void HudBottomBar(const UiSnapshot& snap)
{
    const float barH = ImGui::GetFrameHeight() + Ui(8.0f);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - barH);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    dl->AddRectFilled(ImVec2(p.x - st.WindowPadding.x, p.y),
        ImVec2(p.x + w + st.WindowPadding.x, p.y + barH),
        IM_COL32(0, 0, 0, 225));
    dl->AddLine(ImVec2(p.x - st.WindowPadding.x, p.y),
        ImVec2(p.x + w + st.WindowPadding.x, p.y), IM_COL32(255, 255, 255, 145), Ui(1.0f));

    ImGui::Dummy(ImVec2(0.0f, Ui(3.0f)));
    const char* pl = "";
    ImVec4 pc = kHudMuted;
    PhaseInfo(snap.phase, pl, pc);
    ImGui::TextColored(pc, "%s", pl);
    ImGui::SameLine();
    ImGui::TextColored(kHudMuted, "  %s", WideToUtf8(snap.status).c_str());

    const std::string versionText = "Launcher " + WideToUtf8(snap.launcherVersion)
        + "  |  DLL " + WideToUtf8(snap.netplayDllVersion);
    ImGui::SameLine(w - ImGui::CalcTextSize(versionText.c_str()).x);
    ImGui::TextColored(kHudMuted, "%s", versionText.c_str());
}

int PageIndex(const LauncherPage page)
{
    return static_cast<int>(page);
}

void SelectPage(const LauncherPage page)
{
    if (g_page == page)
    {
        return;
    }
    g_page = page;
    g_pageContentOffset = Ui(28.0f);
    g_pageContentAlpha = 0.15f;
    CancelRebind();
}

void DrawPageRailButton(const LauncherPage page, const char* label)
{
    const ImVec2 size(ImGui::GetContentRegionAvail().x, Ui(40.0f));
    ImGui::PushID(PageIndex(page));
    const bool pressed = ImGui::InvisibleButton("page", size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered && g_page != page)
    {
        dl->AddRectFilled(itemMin, itemMax,
            ImGui::GetColorU32(ImVec4(0.16f, 0.80f, 1.0f, 0.09f)));
    }
    const ImVec4 color = g_page == page ? kAccent : (hovered ? ImVec4(0.88f, 0.93f, 1.0f, 1.0f) : kHudMuted);
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float fontScale = font != nullptr && font->FontSize > 0.0f
        ? fontSize / font->FontSize : 1.0f;

    // Centre the glyph ink bounds: ITC Bolt's line box is asymmetric.
    float penX = 0.0f;
    ImVec2 inkMin(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    ImVec2 inkMax(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    bool hasInk = false;
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(label);
         *cursor != 0u;
         ++cursor)
    {
        const ImFontGlyph* glyph = font != nullptr ? font->FindGlyph(*cursor) : nullptr;
        if (glyph == nullptr)
        {
            continue;
        }
        if (glyph->Visible)
        {
            inkMin.x = std::min(inkMin.x, penX + glyph->X0 * fontScale);
            inkMin.y = std::min(inkMin.y, glyph->Y0 * fontScale);
            inkMax.x = std::max(inkMax.x, penX + glyph->X1 * fontScale);
            inkMax.y = std::max(inkMax.y, glyph->Y1 * fontScale);
            hasInk = true;
        }
        penX += glyph->AdvanceX * fontScale;
    }

    ImVec2 textOrigin{};
    const ImVec2 itemCenter((itemMin.x + itemMax.x) * 0.5f, (itemMin.y + itemMax.y) * 0.5f);
    if (hasInk)
    {
        textOrigin = ImVec2(
            itemCenter.x - (inkMin.x + inkMax.x) * 0.5f,
            itemCenter.y - (inkMin.y + inkMax.y) * 0.5f);
    }
    else
    {
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        textOrigin = ImVec2(itemCenter.x - textSize.x * 0.5f, itemCenter.y - textSize.y * 0.5f);
    }
    dl->AddText(font, fontSize, textOrigin, ImGui::GetColorU32(color), label);
    if (pressed)
    {
        SelectPage(page);
    }
    ImGui::PopID();
}

void DrawPageRail()
{
    const float rowH = Ui(40.0f);
    // Step by height plus ItemSpacing, or the selection strip drifts above its label.
    const float rowStride = rowH + ImGui::GetStyle().ItemSpacing.y;
    const float targetY = static_cast<float>(PageIndex(g_page)) * rowStride;
    const float ease = 1.0f - std::exp(-14.0f * ImGui::GetIO().DeltaTime);
    g_pageHighlightY += (targetY - g_pageHighlightY) * ease;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(
        ImVec2(origin.x, origin.y + g_pageHighlightY),
        ImVec2(origin.x + width, origin.y + g_pageHighlightY + rowH),
        IM_COL32(30, 155, 190, 105));
    dl->AddRectFilled(
        ImVec2(origin.x, origin.y + g_pageHighlightY),
        ImVec2(origin.x + Ui(3.0f), origin.y + g_pageHighlightY + rowH),
        ImGui::GetColorU32(kAccent));

    DrawPageRailButton(LauncherPage::Play, "PLAY ONLINE");
    DrawPageRailButton(LauncherPage::Practice, "PRACTICE");
    DrawPageRailButton(LauncherPage::Controls, "CONTROLS");
    DrawPageRailButton(LauncherPage::Settings, "SETTINGS");
}

void DrawMainWindow(CasterApp& app, const UiSnapshot& snap)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("EFZ FF Netplay", nullptr, flags);

    HudHeader(snap);

    const float bottomBarH = ImGui::GetFrameHeight() + Ui(12.0f);
    const float contentH = std::max(Ui(120.0f),
        ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - bottomBarH);

    ImGui::BeginChild("page_rail", ImVec2(Ui(132.0f), contentH), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawPageRail();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("page_content", ImVec2(0.0f, contentH), true);
    const float ease = 1.0f - std::exp(-15.0f * ImGui::GetIO().DeltaTime);
    g_pageContentOffset += (0.0f - g_pageContentOffset) * ease;
    g_pageContentAlpha += (1.0f - g_pageContentAlpha) * ease;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_pageContentOffset);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
        ImGui::GetStyle().Alpha * std::clamp(g_pageContentAlpha, 0.0f, 1.0f));
    ImGui::PushTextWrapPos(0.0f);

    switch (g_page)
    {
    case LauncherPage::Play:
        HudTitle("ONLINE MATCH");
        DrawPlayTab(app, snap);
        break;
    case LauncherPage::Practice:
        HudTitle("OFFLINE PRACTICE");
        DrawPracticeTab(app, snap);
        break;
    case LauncherPage::Controls:
        HudTitle("CONTROL SETUP");
        if (OpenLauncherControllerCount() < static_cast<int>(g_launcherControllers.size()))
        {
            OpenLauncherControllers(false);
        }
        DrawControlsTab(app, snap);
        break;
    case LauncherPage::Settings:
        HudTitle("LAUNCHER SETTINGS");
        DrawSettingsTab(app, snap);
        break;
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::EndChild();

    // App-level, so changing tabs cannot hide the required choice.
    DrawConnectionPrompt(app, snap);

    HudBottomBar(snap);
    ImGui::End();
}

}

template <typename T>
void ReleaseCom(T*& object)
{
    if (object != nullptr)
    {
        object->Release();
        object = nullptr;
    }
}

SDL_Texture* CreatePngTexture(const void* data, const std::size_t size)
{
    if (data == nullptr || size == 0u || size > std::numeric_limits<DWORD>::max())
    {
        return nullptr;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
    {
        return nullptr;
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    SDL_Texture* texture = nullptr;

    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
    {
        result = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(result))
    {
        result = stream->InitializeFromMemory(
            static_cast<BYTE*>(const_cast<void*>(data)),
            static_cast<DWORD>(size));
    }
    if (SUCCEEDED(result))
    {
        result = factory->CreateDecoderFromStream(
            stream,
            nullptr,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
    }
    if (SUCCEEDED(result))
    {
        result = decoder->GetFrame(0u, &frame);
    }
    if (SUCCEEDED(result))
    {
        result = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(result))
    {
        result = converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }

    UINT width = 0u;
    UINT height = 0u;
    if (SUCCEEDED(result))
    {
        result = converter->GetSize(&width, &height);
    }

    const std::uint64_t pitch64 = static_cast<std::uint64_t>(width) * 4u;
    const std::uint64_t bytes64 = pitch64 * static_cast<std::uint64_t>(height);
    if (SUCCEEDED(result)
        && width > 0u
        && height > 0u
        && width <= static_cast<UINT>(std::numeric_limits<int>::max())
        && height <= static_cast<UINT>(std::numeric_limits<int>::max())
        && pitch64 <= std::numeric_limits<UINT>::max()
        && bytes64 <= std::numeric_limits<UINT>::max())
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes64));
        result = converter->CopyPixels(
            nullptr,
            static_cast<UINT>(pitch64),
            static_cast<UINT>(bytes64),
            pixels.data());
        if (SUCCEEDED(result))
        {
            SDL_Surface* surface = SDL_CreateSurfaceFrom(
                static_cast<int>(width),
                static_cast<int>(height),
                SDL_PIXELFORMAT_RGBA32,
                pixels.data(),
                static_cast<int>(pitch64));
            if (surface != nullptr)
            {
                texture = SDL_CreateTextureFromSurface(g_renderer, surface);
                SDL_DestroySurface(surface);
            }
        }
    }

    ReleaseCom(converter);
    ReleaseCom(frame);
    ReleaseCom(decoder);
    ReleaseCom(stream);
    ReleaseCom(factory);
    if (uninitializeCom)
    {
        CoUninitialize();
    }
    return texture;
}

void LoadBannerTexture()
{
    if (HMODULE module = GetModuleHandleW(nullptr))
    {
        if (HRSRC resource = FindResourceA(
                module,
                "TRIALBANNER",
                reinterpret_cast<LPCSTR>(RT_RCDATA)))
        {
            HGLOBAL handle = LoadResource(module, resource);
            const void* data = handle != nullptr ? LockResource(handle) : nullptr;
            const DWORD size = SizeofResource(module, resource);
            g_bannerTexture = CreatePngTexture(data, size);
        }
    }

    if (g_bannerTexture == nullptr)
    {
        const std::filesystem::path directory = util::ExeDirectory();
        const std::filesystem::path candidates[] = {
            directory / L"assets" / L"banner.png",
            directory / L"banner.png",
        };
        for (const std::filesystem::path& candidate : candidates)
        {
            std::size_t fileSize = 0u;
            void* fileData = SDL_LoadFile(WideToUtf8(candidate.wstring()).c_str(), &fileSize);
            if (fileData == nullptr)
            {
                continue;
            }
            g_bannerTexture = CreatePngTexture(fileData, fileSize);
            SDL_free(fileData);
            if (g_bannerTexture != nullptr)
            {
                break;
            }
        }
    }

    if (g_bannerTexture != nullptr)
    {
        SDL_SetTextureScaleMode(g_bannerTexture, SDL_SCALEMODE_LINEAR);
    }
    else
    {
        SessionLog::Warn(L"ui", L"The embedded banner PNG could not be decoded");
    }
}

bool Initialize(const float userScale)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK))
    {
        SessionLog::Error(L"ui", L"SDL initialization failed: %s", Utf8ToWide(SDL_GetError()).c_str());
        return false;
    }

    OpenLauncherControllers(true);

    g_window = SDL_CreateWindow(
        "EFZ FF Netplay",
        static_cast<int>(kDefaultWindowWidth),
        static_cast<int>(kDefaultWindowHeight),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (g_window == nullptr)
    {
        SessionLog::Error(L"ui", L"Window creation failed: %s", Utf8ToWide(SDL_GetError()).c_str());
        SDL_Quit();
        return false;
    }

    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (g_renderer == nullptr)
    {
        const std::wstring acceleratedError = Utf8ToWide(SDL_GetError());
        SessionLog::Warn(
            L"ui",
            L"Default renderer failed (%s); trying the SDL software renderer",
            acceleratedError.c_str());
        g_renderer = SDL_CreateRenderer(g_window, "software");
        if (g_renderer == nullptr)
        {
            SessionLog::Error(
                L"ui",
                L"Software renderer failed: %s",
                Utf8ToWide(SDL_GetError()).c_str());
            SDL_DestroyWindow(g_window);
            g_window = nullptr;
            SDL_Quit();
            return false;
        }
    }

    g_renderVsyncActive = SDL_SetRenderVSync(g_renderer, 1);
    g_nextUnthrottledFrameMs = 0u;
    if (!g_renderVsyncActive)
    {
        SessionLog::Warn(
            L"ui",
            L"Renderer VSync is unavailable; enabling the 60 Hz launcher fallback cap");
    }

    g_userScale = std::clamp(userScale, 0.75f, 2.0f);
    const float displayScale = SDL_GetWindowDisplayScale(g_window);
    g_displayScale = displayScale > 0.0f ? displayScale : 1.0f;
    g_uiScale = EffectiveUiScaleForDisplay(
        g_displayScale * g_userScale);
    g_responsiveScalePending = false;
    g_lastWindowResizeMs = 0u;
    int minimumWidth = 0;
    int minimumHeight = 0;
    ScaledWindowLimits(
        g_uiScale,
        &minimumWidth,
        &minimumHeight,
        nullptr,
        nullptr);
    SDL_SetWindowMinimumSize(g_window, minimumWidth, minimumHeight);
    SDL_SetWindowSize(
        g_window,
        static_cast<int>(std::lround(kDefaultWindowWidth * g_uiScale)),
        static_cast<int>(std::lround(kDefaultWindowHeight * g_uiScale)));
    CenterWindowOnCurrentDisplay(
        static_cast<int>(std::lround(kDefaultWindowWidth * g_uiScale)),
        static_cast<int>(std::lround(kDefaultWindowHeight * g_uiScale)));
    SessionLog::Step(
        L"ui",
        L"Scale initialized display=%.3f user=%.3f effective=%.3f window=%dx%d",
        displayScale,
        g_userScale,
        g_uiScale,
        static_cast<int>(std::lround(kDefaultWindowWidth * g_uiScale)),
        static_cast<int>(std::lround(kDefaultWindowHeight * g_uiScale)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    LoadHudFonts(g_uiScale);
    ApplyCasterTheme(g_uiScale);
    ipinfo::StartDetection();

    if (!ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer))
    {
        SessionLog::Error(L"ui", L"ImGui SDL backend initialization failed");
        ImGui::DestroyContext();
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_renderer = nullptr;
        g_window = nullptr;
        SDL_Quit();
        return false;
    }

    if (!ImGui_ImplSDLRenderer3_Init(g_renderer))
    {
        SessionLog::Error(L"ui", L"ImGui renderer backend initialization failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_renderer = nullptr;
        g_window = nullptr;
        SDL_Quit();
        return false;
    }
    g_rendererBackendReady = true;

    LoadBannerTexture();

    SDL_ShowWindow(g_window);
    g_running = true;
    return true;
}

void Shutdown()
{
    CloseLauncherControllers();
    g_rendererBackendReady = false;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (g_bannerTexture != nullptr)
    {
        SDL_DestroyTexture(g_bannerTexture);
        g_bannerTexture = nullptr;
    }
    if (g_renderer != nullptr)
    {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = nullptr;
    }
    if (g_window != nullptr)
    {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
    g_running = false;
    g_renderVsyncActive = false;
    g_nextUnthrottledFrameMs = 0u;
}

bool RunFrame(CasterApp& app)
{
    if (!g_running)
    {
        return false;
    }

    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
        if (ProcessControlRebind(event, app))
        {
            continue;
        }

        if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_JOYSTICK_ADDED)
        {
            CancelRebind();
            OpenLauncherControllers(true);
        }
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED || event.type == SDL_EVENT_JOYSTICK_REMOVED)
        {
            bool removedOpenController = false;
            for (const LauncherController& controller : g_launcherControllers)
            {
                removedOpenController = removedOpenController
                    || (controller.joystick != nullptr
                        && event.jdevice.which == SDL_GetJoystickID(controller.joystick));
            }
            if (removedOpenController)
            {
                CancelRebind();
                OpenLauncherControllers(true);
            }
        }

        if (event.type == SDL_EVENT_WINDOW_RESIZED
            || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
            || event.type == SDL_EVENT_WINDOW_MAXIMIZED
            || event.type == SDL_EVENT_WINDOW_RESTORED)
        {
            g_responsiveScalePending = true;
            g_lastWindowResizeMs = SDL_GetTicks();
        }

        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT)
        {
            g_running = false;
            return false;
        }
    }

    static bool buffersSynced = false;
    if (!buffersSynced)
    {
        SyncConfigBuffers(app.Config().Mutable());
        buffersSynced = true;
    }

    const UiSnapshot snap = app.Snapshot();

    const float configuredUserScale = std::clamp(
        static_cast<float>(app.Config().Get().uiScalePercent) / 100.0f,
        0.75f,
        2.0f);
    const float rawDisplayScale = SDL_GetWindowDisplayScale(g_window);
    const float displayScale = rawDisplayScale > 0.0f ? rawDisplayScale : 1.0f;
    const bool configuredScaleChanged =
        std::abs(configuredUserScale - g_userScale) >= 0.001f;
    const bool displayScaleChanged = std::abs(displayScale - g_displayScale) >= 0.01f;
    const float desiredScale = EffectiveUiScaleForDisplay(displayScale * configuredUserScale);
    if (configuredScaleChanged || displayScaleChanged)
    {
        g_userScale = configuredUserScale;
        g_displayScale = displayScale;
        ApplyUiScale(desiredScale, true);
        g_responsiveScalePending = true;
        g_lastWindowResizeMs = SDL_GetTicks();
    }
    else
    {
        const Uint64 now = SDL_GetTicks();
        if (!g_responsiveScalePending
            || now - g_lastWindowResizeMs >= kResponsiveScaleSettleMs)
        {
            g_responsiveScalePending = false;
            ApplyUiScale(ResponsiveUiScaleForWindow(g_uiScale), false);
        }
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    DrawMainWindow(app, snap);

    ImGui::Render();
    SDL_SetRenderDrawColor(g_renderer, 14, 16, 22, 255);
    SDL_RenderClear(g_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_RenderPresent(g_renderer);
    if (!g_renderVsyncActive)
    {
        const Uint64 now = SDL_GetTicks();
        if (g_nextUnthrottledFrameMs == 0u
            || now > g_nextUnthrottledFrameMs + kUnthrottledFrameIntervalMs)
        {
            g_nextUnthrottledFrameMs = now + kUnthrottledFrameIntervalMs;
        }
        else
        {
            if (now < g_nextUnthrottledFrameMs)
            {
                SDL_Delay(static_cast<Uint32>(g_nextUnthrottledFrameMs - now));
            }
            g_nextUnthrottledFrameMs += kUnthrottledFrameIntervalMs;
        }
    }
    return true;
}

}
