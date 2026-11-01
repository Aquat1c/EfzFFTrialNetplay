#include "caster_ui.h"

#include "caster_input.h"
#include "caster_session.h"
#include "caster_telemetry.h"
#include "menu_nav.h"
#include "netplay_bootstrap.h"
#include "netplay_navigation.h"
#include "netplay_sync.h"
#include "rollback.h"
#include "collision_display.h"
#include "debug_log.h"
#include "framestep.h"
#include "savestate.h"
#include "control_config.h"
#include "sdl_input.h"
#include "trial_input_codec.h"
#include "trial_addresses.h"
#include "trial_protocol.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace thv::ui
{
namespace
{

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_visible{true};

ImGuiContext* g_imguiContext = nullptr;
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;

bool g_verboseUi = false;
bool g_autoScrollLog = true;
bool g_showHotkeys = true;
bool g_alwaysOnTop = true;
float g_displayScale = 1.0f;
std::uint32_t g_pollCycles = 0;
std::uint32_t g_renderCycles = 0;
std::uint32_t g_lastRenderLogTick = 0;

const char* CpuLabel(int cpuFlag)
{
    return cpuFlag == 0 ? "human" : "cpu";
}

const char* LayerLabel(std::uint8_t mask, std::uint8_t bit, const char* name)
{
    return (mask & bit) != 0u ? name : "-";
}

void DrawPlayerTable(const char* title, const telemetry::PlayerInputSnapshot& player)
{
    if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    if (ImGui::BeginTable(title, 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [&](const char* label, const char* fmt, ...)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            char buf[256] = {};
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            ImGui::TextUnformatted(buf);
        };

        row("Character", "%u", static_cast<unsigned>(player.charId));
        row("CPU flag", "%s (%d)", CpuLabel(player.cpuFlag), player.cpuFlag);
        row("Facing", "%d", static_cast<int>(player.facing));
        row("Wire mask", "0x%02X", static_cast<unsigned>(player.wireMask));
        row("Horiz CE06", "0x%02X", static_cast<unsigned>(player.horiz));
        row("Vert CE07", "0x%02X", static_cast<unsigned>(player.vert));
        row("Btn A-D", "%02X %02X %02X %02X",
            static_cast<unsigned>(player.buttonA),
            static_cast<unsigned>(player.buttonB),
            static_cast<unsigned>(player.buttonC),
            static_cast<unsigned>(player.buttonD));
        row("History idx", "%u", static_cast<unsigned>(player.historyIndex));
        row("Packed word", "0x%04X", static_cast<unsigned>(player.lastPackedWord));
        ImGui::EndTable();
    }
}

void DrawHotkeysPanel()
{
    if (!ImGui::CollapsingHeader("Hotkeys"))
    {
        return;
    }

    ImGui::BulletText("F1 — show/hide this window");
    ImGui::BulletText("PgUp / PgDn / Home — hit / hurt / collision layers");
    ImGui::BulletText("Space / P — framestep pause / step");
    ImGui::BulletText("T — swap human side");
    ImGui::BulletText("U / J — practice save / load (slots 0–7)");
    ImGui::BulletText(", / . — prev / next practice slot");
    ImGui::BulletText("F6 / F7 — debug save / load (trial_savestates/debug.thvs)");
    ImGui::BulletText("L — append snapshot to log file");
    ImGui::BulletText("O — toggle verbose input logging");
}

void DrawOverview(const telemetry::RuntimeSnapshot& snap)
{
    if (!ImGui::CollapsingHeader("Screen & session", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    if (ImGui::BeginTable("overview", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [&](const char* k, const char* v)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(v);
        };

        char buf[128] = {};
        std::snprintf(
            buf,
            sizeof(buf),
            "%u (%s)",
            static_cast<unsigned>(snap.screenMode),
            telemetry::ScreenModeName(snap.screenMode));
        row("Screen mode", buf);

        std::snprintf(buf, sizeof(buf), "0x%08X", snap.screenSubState);
        row("Sub-state", buf);
        row("Battle active", snap.battleActive ? "yes" : "no");
        row("DI active", snap.diActive ? "yes" : "no");

        std::snprintf(buf, sizeof(buf), "P%d", snap.humanSide + 1);
        row("Human side", buf);
        row("Framestep", snap.framestepPaused ? "PAUSED" : "RUNNING");
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(snap.localFrame));
        row("Local frame", buf);

        const std::uint8_t layers = GetLayerMask();
        std::snprintf(
            buf,
            sizeof(buf),
            "0x%X  hit=%s hurt=%s col=%s",
            static_cast<unsigned>(layers),
            LayerLabel(layers, kLayerHit, "on"),
            LayerLabel(layers, kLayerHurt, "on"),
            LayerLabel(layers, kLayerCollision, "on"));
        row("Collision layers", buf);

        std::snprintf(
            buf,
            sizeof(buf),
            "practice %d  (saves=%u loads=%u)",
            snap.saveSlot,
            snap.saveCount,
            snap.loadCount);
        row("Practice slot", buf);
        row("Debug file", HasDebugSavestate() ? "debug.thvs present" : "debug.thvs missing");

        ImGui::EndTable();
    }
}

void DrawDeterminism(const telemetry::RuntimeSnapshot& snap)
{
    if (!ImGui::CollapsingHeader("Determinism", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    if (ImGui::BeginTable("det", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [&](const char* k, const char* fmt, ...)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k);
            ImGui::TableSetColumnIndex(1);
            char buf[128] = {};
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            ImGui::TextUnformatted(buf);
        };

        row("FPU pinned CW", "0x%04X", static_cast<unsigned>(snap.fpuCapturedCw));
        row("FPU last observed", "0x%04X", static_cast<unsigned>(snap.fpuLastObservedCw));
        row("FPU mismatches", "%u", snap.fpuMismatchCount);
        row("RNG shadow", "0x%08X", snap.rngShadow);
        row("RNG last seed", "0x%08X", snap.rngLastSeed);
        row("srand / rand calls", "%u / %u", snap.rngSrandCalls, snap.rngRandCalls);
        row(
            "RNG hooks",
            "iatR=%d iatS=%d staticR=%d staticS=%d",
            snap.rngIatRand ? 1 : 0,
            snap.rngIatSrand ? 1 : 0,
            snap.rngStaticRand ? 1 : 0,
            snap.rngStaticSrand ? 1 : 0);

        ImGui::EndTable();
    }
}

const char* RollbackSubStateLabel(rollback::RollbackSubState state)
{
    switch (state)
    {
    case rollback::RollbackSubState::InitCatchUp:
        return "init";
    case rollback::RollbackSubState::MenuIdle:
        return "menu";
    case rollback::RollbackSubState::PreRoll:
        return "pre-roll";
    case rollback::RollbackSubState::Battle:
        return "battle";
    default:
        return "other";
    }
}

const char* RollbackPhaseLabel(rollback::TransportPhase phase)
{
    switch (phase)
    {
    case rollback::TransportPhase::WaitingRemote:
        return "wait-remote";
    case rollback::TransportPhase::Predicting:
        return "predict";
    case rollback::TransportPhase::BatchCatchUp:
        return "batch";
    case rollback::TransportPhase::Reloading:
        return "reload";
    default:
        return "idle";
    }
}

void DrawRollbackPanel()
{
    if (!ImGui::CollapsingHeader("Rollback engine"))
    {
        return;
    }

    const rollback::Status rb = rollback::GetStatus();

    if (!rb.compiledIn)
    {
        ImGui::TextUnformatted("Module not compiled in.");
        return;
    }

    const bool netplaySession =
        caster::IsNetplayActive() && netplay::IsSessionActive();
    if (netplaySession)
    {
        if (netplay::IsRollbackRequired())
        {
            ImGui::TextDisabled("Rollback netplay session (negotiated at connect).");
            ImGui::Text("Rollback: %s", rb.enabled ? "ON" : "OFF");
            ImGui::Text(
                "Negotiated: %s",
                netplay::IsRollbackNegotiated() ? "yes" : "pending");
        }
        else
        {
            ImGui::TextDisabled("Lockstep netplay — SyncData only (rollback disabled).");
            ImGui::Text("Rollback engine: OFF");
        }
    }
    else
    {
        ImGui::TextDisabled("Enable this only for offline rollback testing.");
        bool enableUi = rb.enabled;
        if (ImGui::Checkbox("Enable rollback", &enableUi))
        {
            rollback::SetEnabled(enableUi);
        }
    }

    if (ImGui::BeginTable("rollback_status", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [&](const char* k, const char* fmt, ...)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k);
            ImGui::TableSetColumnIndex(1);
            char buf[128] = {};
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            ImGui::TextUnformatted(buf);
        };

        row("Active", "%s", rb.active ? "yes" : "no");
        row("Battle screen", "%s", rb.battleScreen ? "yes" : "no");
        row("Sub-state", "%s", RollbackSubStateLabel(rb.subState));
        row("Phase", "%s", RollbackPhaseLabel(rb.phase));
        row("Local side", "P%d", rb.localSide + 1);
        row("Input delay", "%d (adj %d)", rb.inputDelay, rb.delayAdjust);
        row("Frames", "cur=%u sim=%u confirmed=%u",
            rb.currentFrame,
            rb.simFrame,
            rb.confirmedFrame);
        row("Input history", "local=%zu remote=%zu predicted=%zu",
            rb.localInputCount,
            rb.remoteInputCount,
            rb.predictedInputCount);
        row("Ring slots", "%zu / %d occupied", rb.occupiedRingSlots, rollback::kFrameRingSize);
        row("Last batch", "%u frames", rb.lastBatchFrames);
        row("Rollbacks", "%u (last reload @ %u)", rb.rollbacksTotal, rb.lastReloadFrame);
        row(
            "Input inject",
            "%s via input poll hook",
            rollback::IsInputAuthorityActive() ? "rollback history" : "caster/SDL");
        ImGui::EndTable();
    }
}

void DrawPacketFlowPanel()
{
    if (!ImGui::CollapsingHeader("Packet flow", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const auto navPhase = netplay::nav::GetPhase();
    const auto packetStage = netplay::nav::GetPacketStage();
    const auto stats = netplay::GetPacketFlowStats();

    ImGui::Text("Bootstrap: %s", netplay::GetBootstrapPhaseName());
    ImGui::Text("Navigation: %s", netplay::nav::PhaseName(navPhase));
    ImGui::Text("Packet stage: %s", netplay::nav::PacketStageName(packetStage));
    ImGui::Text("Netplay session: %s", netplay::IsSessionActive() ? "active" : "idle");
    ImGui::Text(
        "Determinism: %s seed=0x%08X",
        netplay::IsDeterminismSynced() ? "synced" : "pending",
        netplay::GetSyncedRngSeed());
    ImGui::Text(
        "Net mode: %s%s",
        netplay::IsRollbackRequired() ? "rollback" : "lockstep",
        netplay::IsRollbackRequired()
            ? (netplay::IsRollbackNegotiated() ? " (negotiated)" : " (pending)")
            : "");
    if (netplay::IsNetplayAborted())
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
            "%s",
            netplay::GetNetplayAbortReason());
    }

    const auto lockStats = netplay::GetLockstepStats();
    ImGui::Text(
        "Lockstep: local=%u remote=%u lead=%u stalls=%u",
        lockStats.localFrames,
        lockStats.remoteFrames,
        lockStats.lastRemoteLead,
        lockStats.stallCount);
    ImGui::Text("Menu screen: %s", menu::NavScreenName(menu::DetectNavScreen()));

    if (ImGui::BeginTable("pkt_stats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [&](const char* k, std::uint32_t v)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", v);
        };

        row("seed sent", stats.seedPacketsSent);
        row("seed recv", stats.seedPacketsRecv);
        row("launch-go sent", stats.launchGoSent);
        row("launch-go recv", stats.launchGoRecv);
        row("SyncData sent", stats.syncPacketsSent);
        row("SyncData recv", stats.syncPacketsRecv);
        row("sync applied", stats.syncFramesApplied);
        row("screen transitions", stats.screenTransitions);
        ImGui::EndTable();
    }

    if (ImGui::Button("Force VS Human now"))
    {
        netplay::ForceEnterVsHuman();
        log::Step("ui", "user requested Force VS Human");
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump nav state to log"))
    {
        log::Info(
            "ui",
            "nav bootstrap=%s phase=%s stage=%s syncSent=%u syncRecv=%u",
            netplay::GetBootstrapPhaseName(),
            netplay::nav::PhaseName(navPhase),
            netplay::nav::PacketStageName(packetStage),
            stats.syncPacketsSent,
            stats.syncPacketsRecv);
    }
}

void DrawUiDiagnosticsPanel()
{
    if (!ImGui::CollapsingHeader("UI diagnostics"))
    {
        return;
    }

    ImGui::Checkbox("Always on top", &g_alwaysOnTop);
    if (g_window != nullptr)
    {
        SDL_SetWindowAlwaysOnTop(g_window, g_alwaysOnTop);
    }

    ImGui::Text("Display scale: %.2f", g_displayScale);
    ImGui::Text("Poll cycles: %u", g_pollCycles);
    ImGui::Text("Render cycles: %u", g_renderCycles);
    ImGui::Text("Window visible: %s", g_visible.load() ? "yes" : "no");

    if (g_window != nullptr)
    {
        int w = 0;
        int h = 0;
        int pw = 0;
        int ph = 0;
        SDL_GetWindowSize(g_window, &w, &h);
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        ImGui::Text("Window logical: %dx%d  pixels: %dx%d", w, h, pw, ph);
    }
}

void DrawCasterInputPanel()
{
    if (!ImGui::CollapsingHeader("Caster transport", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    int mode = static_cast<int>(caster::GetTransportMode());
    const char* modes[] = {"Offline (SDL local)", "Loopback test", "Netplay (UDP SyncData)"};
    if (ImGui::Combo("Mode", &mode, modes, 3))
    {
        caster::SetTransportMode(static_cast<caster::TransportMode>(mode));
    }

    ImGui::Text("Bridge active: %s", caster::IsBridgeActive() ? "yes" : "no");
    ImGui::Text("Local side: P%d", caster::GetLocalSide() + 1);
    ImGui::Text("Remote side: P%d", caster::GetRemoteSide() + 1);

    const proto::PlayerInputRecord local = caster::GetLocalRecord();
    const proto::PlayerInputRecord remote = caster::GetRemoteRecord();
    ImGui::Text("Local wire mask:  0x%02X", static_cast<unsigned>(local.playerAction));
    ImGui::Text("Remote wire mask: 0x%02X%s",
        static_cast<unsigned>(remote.playerAction),
        caster::HasRemoteInput() ? "" : " (none)");

  {
    const auto& queue = caster::GetLocalQueue();
    ImGui::Text("Local queue depth: %u", queue.writeIndex);
    if (queue.writeIndex > 0)
    {
        const std::uint32_t last = queue.writeIndex - 1u;
        const auto& slot = queue.slots[last % proto::kMaxQueueSlot];
        ImGui::Text("Last queued action: 0x%02X sys=0x%02X",
            static_cast<unsigned>(slot.playerAction),
            static_cast<unsigned>(slot.systemState));
    }
  }

    ImGui::TextWrapped(
        "Wire format: UDLRABCD. Pad bindings apply to local player only.");

    ImGui::SeparatorText("Bindings verifier");
    ImGui::Text(
        "Gamepad: %s",
        input::HasGamepad() ? input::GamepadName() : "(none)");
    ImGui::Text("Gameplay side: P%d", caster::GetLocalSide() + 1);

    const int localPlayer = caster::LocalBindingPlayerIndex();
    const char* slotNames[codec::kSlotCount] = {
        "Up", "Down", "Left", "Right", "A", "B", "C", "D"};
  {
    const std::uint8_t liveMask = codec::SampleSdlWireMaskForPlayer(localPlayer);
    ImGui::Text("Live local controls: 0x%02X", static_cast<unsigned>(liveMask));
    std::uint32_t slots[codec::kSlotCount] = {};
    codec::UnpackWireMask(liveMask, slots);
    ImGui::Text(
        "Bits: %s%s%s%s %s%s%s%s",
        slots[0] ? "U" : ".",
        slots[1] ? "D" : ".",
        slots[2] ? "L" : ".",
        slots[3] ? "R" : ".",
        slots[4] ? "A" : ".",
        slots[5] ? "B" : ".",
        slots[6] ? "C" : ".",
        slots[7] ? "D" : ".");
  }

    if (ImGui::BeginTable("ingame_bind_verify", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Pad");
        ImGui::TableSetupColumn("Key live");
        ImGui::TableSetupColumn("Pad live");
        ImGui::TableHeadersRow();

        for (int slot = 0; slot < codec::kSlotCount; ++slot)
        {
            const SDL_Scancode key = controls::KeyForPlayer(localPlayer, slot);
            const controls::PadBinding& pad = controls::PadForPlayer(localPlayer, slot);
            const bool keyDown = input::IsSlotDownForPlayer(slot, localPlayer);
            const bool padDown = input::IsGamepadSlotDownForPlayer(localPlayer, slot);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", slotNames[slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%s", SDL_GetScancodeName(key));
            ImGui::TableNextColumn();
            if (pad.active)
            {
                if (pad.kind == controls::PadSourceKind::GamepadAxis)
                {
                    ImGui::Text("axis %d (%s)", static_cast<int>(pad.gamepadAxis),
                        pad.direction < 0 ? "-" : "+");
                }
                else if (pad.kind == controls::PadSourceKind::GamepadButton)
                {
                    const char* padName = SDL_GetGamepadStringForButton(pad.button);
                    ImGui::Text("%s", padName != nullptr ? padName : "?");
                }
                else
                {
                    ImGui::Text("raw %d:%d", static_cast<int>(pad.kind), pad.index);
                }
            }
            else
            {
                ImGui::TextDisabled("none");
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(
                keyDown ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                keyDown ? "ON" : "off");
            ImGui::TableNextColumn();
            ImGui::TextColored(
                padDown ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                pad.active ? (padDown ? "ON" : "off") : "—");
        }
        ImGui::EndTable();
    }
}

void DrawHooksPanel()
{
    if (!ImGui::CollapsingHeader("Hook sites"))
    {
        return;
    }

    ImGui::Text("battleTick     EFZ+0x%05X", static_cast<unsigned>(trial::VaToRva(trial::kVaBattleTick)));
    ImGui::Text("inputPoll      EFZ+0x%05X", static_cast<unsigned>(trial::VaToRva(trial::kVaPollPlayerInput)));
    ImGui::Text("collisionDraw  EFZ+0x%05X", static_cast<unsigned>(trial::VaToRva(trial::kVaDrawCollisionBoxes)));
    ImGui::Text("srand site     EFZ+0x%05X", 0x5C76Bu);
    ImGui::Text("P1 / P2        EFZ+0x%05X / EFZ+0x%05X",
        static_cast<unsigned>(trial::VaToRva(trial::kVaP1Player)),
        static_cast<unsigned>(trial::VaToRva(trial::kVaP2Player)));
}

void DrawLogPanel()
{
    if (!ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const bool fileLoggingEnabled = log::IsFileLoggingEnabled();
    if (fileLoggingEnabled)
    {
        ImGui::TextDisabled("File: %s", log::SessionLogPath().c_str());
    }
    else
    {
        ImGui::TextDisabled("File logging is off. Enable Diagnostics in the launcher to keep logs.");
        if (ImGui::Button("Enable file logging for this session"))
        {
            if (!log::EnableFileLogging())
            {
                log::Error("ui", "could not enable file logging");
            }
        }
    }
    ImGui::Checkbox("Auto-scroll", &g_autoScrollLog);
    ImGui::SameLine();
    ImGui::BeginDisabled(!fileLoggingEnabled);
    if (ImGui::Button("Copy snapshot to log"))
    {
        telemetry::DumpFullSnapshot("manual (UI button)");
    }
    ImGui::EndDisabled();

    std::string logText;
    log::CopyLogText(&logText);

    ImGui::BeginChild("log_scroll", ImVec2(0.0f, 220.0f), ImGuiChildFlags_Border);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::TextUnformatted(logText.c_str());
    ImGui::PopStyleVar();
    if (g_autoScrollLog)
    {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void DrawPracticeTab(const telemetry::RuntimeSnapshot& snap)
{
    ImGui::TextWrapped(
        "Hitbox viewer, framestep, and savestates live in netplay.dll (injected with EFZ). "
        "They only run during battle (screen mode 1) — not on main menu or char select.");

    if (!snap.battleActive)
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
            "Not in battle — overlays and framestep are idle until mode=1.");
    }

    ImGui::Separator();
    if (ImGui::BeginTable("practice_layers", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [&](const char* k, const char* v)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(v);
        };

        const std::uint8_t layers = GetLayerMask();
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(layers));
        row("Layer mask", buf);
        row("Hit (PgUp)", (layers & kLayerHit) != 0u ? "ON" : "off");
        row("Hurt (PgDn)", (layers & kLayerHurt) != 0u ? "ON" : "off");
        row("Collision (Home)", (layers & kLayerCollision) != 0u ? "ON" : "off");
        row("Framestep", snap.framestepPaused ? "PAUSED" : "RUNNING");
        std::snprintf(
            buf,
            sizeof(buf),
            "practice %d (saves=%u loads=%u)",
            snap.saveSlot,
            snap.saveCount,
            snap.loadCount);
        row("Practice slot", buf);
        row("Debug file", HasDebugSavestate() ? "debug.thvs present" : "debug.thvs missing");
        ImGui::EndTable();
    }

    if (ImGui::Button("Toggle hit (PgUp)"))
    {
        ToggleLayer(kLayerHit);
    }
    ImGui::SameLine();
    if (ImGui::Button("Toggle hurt (PgDn)"))
    {
        ToggleLayer(kLayerHurt);
    }
    ImGui::SameLine();
    if (ImGui::Button("Toggle collision (Home)"))
    {
        ToggleLayer(kLayerCollision);
    }

    if (ImGui::Button("All layers on"))
    {
        SetLayerMask(kLayerHit | kLayerHurt | kLayerCollision);
    }
    ImGui::SameLine();
    if (ImGui::Button("All layers off"))
    {
        SetLayerMask(0);
    }

    ImGui::Separator();
    if (ImGui::Button("Framestep pause (Space)"))
    {
        if (!IsFramestepPaused())
        {
            RequestFramestepPause();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Advance 1 frame (P)"))
    {
        RequestFramestepAdvance();
    }

    ImGui::Separator();
    if (ImGui::Button("Debug save (F6)"))
    {
        SaveState(SavestateBank::Debug, -1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Debug load (F7)"))
    {
        LoadState(SavestateBank::Debug, -1);
    }

    ImGui::Separator();
    DrawHotkeysPanel();
}

void DrawTelemetryTab(const telemetry::RuntimeSnapshot& snap)
{
    g_verboseUi = telemetry::VerboseInputLogging();
    if (ImGui::Checkbox("Verbose input logging (O)", &g_verboseUi))
    {
        telemetry::SetVerboseInputLogging(g_verboseUi);
    }

    ImGui::Separator();
    DrawCasterInputPanel();
    DrawRollbackPanel();
    DrawOverview(snap);
    DrawDeterminism(snap);
    DrawPlayerTable("Player 1", snap.p1);
    DrawPlayerTable("Player 2", snap.p2);
    DrawHooksPanel();
    if (g_showHotkeys)
    {
        DrawHotkeysPanel();
    }
}

void DrawDebugTab(const telemetry::RuntimeSnapshot& snap)
{
    DrawPacketFlowPanel();
    DrawUiDiagnosticsPanel();
    DrawPaletteDebugPanel();

    ImGui::Separator();
    if (ImGui::BeginTable("debug_session", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        auto row = [&](const char* k, const char* v)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(v);
        };

        const session::LauncherSession* session = session::GetCachedSession();
        if (session != nullptr)
        {
            char buf[128] = {};
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(session->mode));
            row("Session mode", buf);
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(session->bootstrap));
            row("Bootstrap", buf);
            row("Screen", telemetry::ScreenModeName(snap.screenMode));
        }
        else
        {
            row("Session", "none (offline defaults)");
        }
        ImGui::EndTable();
    }

    DrawLogPanel();
}

void DrawMainWindow()
{
    const telemetry::RuntimeSnapshot snap = telemetry::CaptureRuntimeSnapshot();

    ImGui::SetNextWindowSize(ImVec2(520.0f, 720.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(
        "EFZ FF Netplay Debug",
        nullptr,
        ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Hotkeys", nullptr, &g_showHotkeys);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (ImGui::BeginTabBar("debug_tabs"))
    {
        if (ImGui::BeginTabItem("Practice"))
        {
            DrawPracticeTab(snap);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Telemetry"))
        {
            DrawTelemetryTab(snap);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug"))
        {
            DrawDebugTab(snap);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Text("UI %.1f FPS", ImGui::GetIO().Framerate);
    ImGui::End();
}

}

ImGuiContext* GetImGuiContext()
{
    return g_imguiContext;
}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    g_visible.store(false);
    log::Info("ui", "in-game debug window disabled — use EFZ FF Netplay.exe (Practice page)");
    return true;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    if (g_imguiContext != nullptr)
    {
        ImGui::SetCurrentContext(g_imguiContext);
    }

    if (g_imguiContext != nullptr)
    {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
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

    if (g_imguiContext != nullptr)
    {
        ImGui::DestroyContext(g_imguiContext);
        g_imguiContext = nullptr;
    }

    // No SDL_Quit: it would tear down sdl_input's controller state.
    log::Info("ui", "ImGui caster window shutdown");
}

void PollEvents()
{
    if (!g_initialized.load() || g_window == nullptr || g_imguiContext == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(g_imguiContext);
    ++g_pollCycles;

    SDL_Event event = {};
    int eventsThisPoll = 0;
    while (SDL_PollEvent(&event))
    {
        ++eventsThisPoll;
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT)
        {
            SetVisible(false);
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
            && event.window.windowID == SDL_GetWindowID(g_window))
        {
            SetVisible(false);
        }
    }

    const DWORD now = GetTickCount();
    if (eventsThisPoll > 0 && now - g_lastRenderLogTick > 2000)
    {
        g_lastRenderLogTick = now;
        log::Debug("ui", "PollEvents events=%d cycle=%u", eventsThisPoll, g_pollCycles);
    }

}

void Render()
{
    if (!g_initialized.load() || g_renderer == nullptr || g_window == nullptr || g_imguiContext == nullptr
        || !g_visible.load())
    {
        return;
    }

    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED)
    {
        return;
    }

    ImGui::SetCurrentContext(g_imguiContext);
    ++g_renderCycles;

    const DWORD now = GetTickCount();
    if (now - g_lastRenderLogTick > 5000)
    {
        g_lastRenderLogTick = now;
        log::Debug("ui", "Render cycle=%u fps=%.1f visible=%d", g_renderCycles, ImGui::GetIO().Framerate, g_visible.load() ? 1 : 0);
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    DrawMainWindow();

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_RenderPresent(g_renderer);
}

bool IsVisible()
{
    return g_visible.load();
}

void SetVisible(bool visible)
{
    g_visible.store(visible);
    if (g_window == nullptr)
    {
        return;
    }

    if (visible)
    {
        SDL_ShowWindow(g_window);
        SDL_RaiseWindow(g_window);
        if (g_alwaysOnTop)
        {
            SDL_SetWindowAlwaysOnTop(g_window, true);
        }
    }
    else
    {
        SDL_HideWindow(g_window);
    }
}

}
