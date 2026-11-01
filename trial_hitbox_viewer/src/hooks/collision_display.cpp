#include "collision_display.h"
#include "caster_input.h"
#include "control_config.h"
#include "debug_log.h"
#include "mem_write_log.h"
#include "sdl_input.h"
#include "trial_addresses.h"

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace thv
{
namespace
{

using TransformEntityFn = unsigned int*(__cdecl*)(unsigned int* outRect, int a2, int a3, int a4, int a5, int player);
using TransformProjectileFn = unsigned int*(__cdecl*)(
    unsigned int* outRect,
    int a2,
    int a3,
    int a4,
    int a5,
    int player,
    unsigned char slot);
using DrawPaletteRectFn = int(__cdecl*)(int left, int top, int right, int bottom, char palette);

TransformEntityFn g_transformEntity = nullptr;
TransformProjectileFn g_transformProjectile = nullptr;
DrawPaletteRectFn g_drawPaletteRect = nullptr;

int* g_debugDrawFlag = nullptr;

std::atomic<bool> g_initialized{false};
std::uint8_t g_layerMask = 0;

int g_paletteHurt = trial::kPaletteHurt;
int g_paletteHit = trial::kPaletteHit;
int g_paletteCollision = trial::kPaletteCollision;

int LayerPaletteForBit(const std::uint8_t layerBit)
{
    switch (layerBit)
    {
    case kLayerHurt:
        return g_paletteHurt;
    case kLayerHit:
        return g_paletteHit;
    case kLayerCollision:
        return g_paletteCollision;
    default:
        return trial::kPaletteHurt;
    }
}

const std::uint8_t* PaletteRam()
{
    return static_cast<const std::uint8_t*>(trial::ResolveVa(trial::kVaPaletteEntries));
}

bool ResolveGameFunctions()
{
    if (g_transformEntity != nullptr && g_transformProjectile != nullptr && g_drawPaletteRect != nullptr)
    {
        return true;
    }

    g_transformEntity = reinterpret_cast<TransformEntityFn>(trial::ResolveVa(trial::kVaTransformEntity));
    g_transformProjectile =
        reinterpret_cast<TransformProjectileFn>(trial::ResolveVa(trial::kVaTransformProjectile));
    g_drawPaletteRect = reinterpret_cast<DrawPaletteRectFn>(trial::ResolveVa(trial::kVaDrawPaletteRect));
    g_debugDrawFlag = reinterpret_cast<int*>(trial::ResolveVa(trial::kVaDebugDrawFlag));

    return g_transformEntity != nullptr && g_transformProjectile != nullptr && g_drawPaletteRect != nullptr
        && g_debugDrawFlag != nullptr;
}

void CopyFrameBlob(int player, unsigned char* outBlob)
{
    const auto* const bytes = reinterpret_cast<const unsigned char*>(player);
    const unsigned char subframe = bytes[trial::kOffSubframe];
    const short moveId = *reinterpret_cast<const short*>(bytes + trial::kOffMoveId);
    const void* const src = reinterpret_cast<const void*>(
        player + trial::kFrameBlobStride * subframe + trial::kMoveBlobStride * moveId + trial::kOffFrameBlobBase);
    std::memcpy(outBlob, src, trial::kFrameBlobStride);
}

void CopyProjectileFrameBlob(int player, int slot, unsigned char* outBlob)
{
    const auto* const bytes = reinterpret_cast<const unsigned char*>(player);
    const short subframe = *reinterpret_cast<const short*>(bytes + trial::kOffProjectileSubframe + 2u * slot);
    const short moveId = *reinterpret_cast<const short*>(bytes + trial::kOffProjectileMoveId + 2u * slot);
    const void* const src = reinterpret_cast<const void*>(
        player + trial::kFrameBlobStride * subframe + trial::kMoveBlobStride * moveId + trial::kOffFrameBlobBase);
    std::memcpy(outBlob, src, trial::kFrameBlobStride);
}

void DrawTransformedRects(
    TransformEntityFn transform,
    int player,
    const int* localRects,
    int rectCount,
    int camX,
    int camY,
    int palette)
{
    const int yBase = camY + 210;
    unsigned int outRect[4] = {};
    for (int i = 0; i < rectCount; ++i)
    {
        const int* const local = localRects + (4 * i);
        unsigned int* const transformed = transform(
            outRect,
            local[0],
            local[1],
            local[2],
            local[3],
            player);
        g_drawPaletteRect(
            camX + static_cast<int>(transformed[0]),
            yBase + static_cast<int>(transformed[1]),
            camX + static_cast<int>(transformed[2]),
            yBase + static_cast<int>(transformed[3]),
            static_cast<char>(palette));
    }
}

void DrawTransformedProjectileRects(
    int player,
    unsigned char slot,
    const int* localRects,
    int rectCount,
    int camX,
    int camY,
    int palette)
{
    const int yBase = camY + 210;
    unsigned int outRect[4] = {};
    for (int i = 0; i < rectCount; ++i)
    {
        const int* const local = localRects + (4 * i);
        unsigned int* const transformed = g_transformProjectile(
            outRect,
            local[0],
            local[1],
            local[2],
            local[3],
            player,
            slot);
        g_drawPaletteRect(
            camX + static_cast<int>(transformed[0]),
            yBase + static_cast<int>(transformed[1]),
            camX + static_cast<int>(transformed[2]),
            yBase + static_cast<int>(transformed[3]),
            static_cast<char>(palette));
    }
}

void DrawCharacterLayer(int player, const unsigned char* frameBlob, int camX, int camY, std::uint8_t layerMask)
{
    if ((layerMask & kLayerHurt) != 0)
    {
        DrawTransformedRects(
            g_transformEntity,
            player,
            reinterpret_cast<const int*>(frameBlob + trial::kHurtRectBase),
            trial::kHurtRectCount,
            camX,
            camY,
            g_paletteHurt);
    }

    if ((layerMask & kLayerCollision) != 0)
    {
        const int* const collisionRect = reinterpret_cast<const int*>(frameBlob + trial::kCollisionRectBase);
        unsigned int outRect[4] = {};
        unsigned int* const transformed = g_transformEntity(
            outRect,
            collisionRect[0],
            collisionRect[1],
            collisionRect[2],
            collisionRect[3],
            player);
        const int yBase = camY + 210;
        g_drawPaletteRect(
            camX + static_cast<int>(transformed[0]),
            yBase + static_cast<int>(transformed[1]),
            camX + static_cast<int>(transformed[2]),
            yBase + static_cast<int>(transformed[3]),
            static_cast<char>(g_paletteCollision));
    }

    if ((layerMask & kLayerHit) != 0)
    {
        DrawTransformedRects(
            g_transformEntity,
            player,
            reinterpret_cast<const int*>(frameBlob + trial::kHitRectBase),
            trial::kHitRectCount,
            camX,
            camY,
            g_paletteHit);
    }
}

void DrawProjectileLayer(
    int player,
    unsigned char slot,
    const unsigned char* frameBlob,
    int camX,
    int camY,
    std::uint8_t layerMask)
{
    if ((layerMask & kLayerHurt) != 0 && slot < 5)
    {
        const int extraCount = 5 - slot;
        for (int i = 0; i < extraCount; ++i)
        {
            DrawTransformedProjectileRects(
                player,
                slot,
                reinterpret_cast<const int*>(frameBlob + trial::kHurtRectBase),
                1,
                camX,
                camY,
                g_paletteHurt);
        }
    }

    if ((layerMask & kLayerCollision) != 0)
    {
        const int* const collisionRect = reinterpret_cast<const int*>(frameBlob + trial::kCollisionRectBase);
        DrawTransformedProjectileRects(
            player,
            slot,
            collisionRect,
            trial::kCollisionRectCount,
            camX,
            camY,
            g_paletteCollision);
    }

    if ((layerMask & kLayerHit) != 0)
    {
        DrawTransformedProjectileRects(
            player,
            slot,
            reinterpret_cast<const int*>(frameBlob + trial::kHitRectBase),
            trial::kHitRectCount,
            camX,
            camY,
            g_paletteHit);
    }
}

void DrawProjectileRing(int player, int camX, int camY, std::uint8_t layerMask)
{
    const auto* const bytes = reinterpret_cast<const unsigned char*>(player);
    int ringIndex = bytes[trial::kOffProjectileRingHead];
    const int ringEnd = ringIndex + trial::kProjectileRingSize;

    while (ringIndex < ringEnd)
    {
        const int slot = ringIndex - trial::kProjectileRingSize * (ringIndex >= trial::kProjectileRingSize);
        if (*reinterpret_cast<const unsigned int*>(bytes + trial::kOffProjectileAlive + 4u * slot) != 0)
        {
            unsigned char frameBlob[trial::kFrameBlobStride] = {};
            CopyProjectileFrameBlob(player, slot, frameBlob);
            DrawProjectileLayer(player, static_cast<unsigned char>(slot), frameBlob, camX, camY, layerMask);
        }
        ++ringIndex;
    }
}

}

void ApplyDebugPaletteColors()
{
}

bool Initialize()
{
    if (g_initialized.exchange(true))
    {
        return true;
    }

    if (!ResolveGameFunctions())
    {
        g_initialized = false;
        log::Error("collision", "Initialize failed (game functions unresolved)");
        return false;
    }

    g_paletteHurt = trial::kPaletteHurt;
    g_paletteHit = trial::kPaletteHit;
    g_paletteCollision = trial::kPaletteCollision;
    SetDebugDrawFlag(false);
    log::Step("collision", "Initialize ok (read-only palette indices %d/%d/%d)", g_paletteHurt, g_paletteCollision, g_paletteHit);
    return true;
}

void Shutdown()
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    log::Step("collision", "Shutdown");
    g_layerMask = 0;
    SetDebugDrawFlag(false);
}

void PollHotkeys()
{
    if (!g_initialized.load() || caster::IsNetplayActive() || caster::IsSpectatorActive())
    {
        return;
    }

    const auto pressed = [](const trialnet::shortcuts::Action action) {
        return !controls::ShortcutConflictsWithAnyFightBinding(action)
            && input::WasPressed(controls::ShortcutKey(action));
    };

    if (pressed(trialnet::shortcuts::Action::ToggleHitboxes))
    {
        ToggleLayer(kLayerHit);
        log::Info("collision", "hotkey PgUp — hit layer %s", (g_layerMask & kLayerHit) != 0u ? "ON" : "off");
    }
    if (pressed(trialnet::shortcuts::Action::ToggleHurtboxes))
    {
        ToggleLayer(kLayerHurt);
        log::Info("collision", "hotkey PgDn — hurt layer %s", (g_layerMask & kLayerHurt) != 0u ? "ON" : "off");
    }
    if (pressed(trialnet::shortcuts::Action::ToggleCollision))
    {
        ToggleLayer(kLayerCollision);
        log::Info(
            "collision",
            "hotkey Home — collision layer %s",
            (g_layerMask & kLayerCollision) != 0u ? "ON" : "off");
    }

    SetDebugDrawFlag(g_layerMask != 0);
}

std::uint8_t GetLayerMask()
{
    return g_layerMask;
}

void SetLayerMask(const std::uint8_t mask)
{
    g_layerMask = mask;
    SetDebugDrawFlag(g_layerMask != 0);
}

void ToggleLayer(const std::uint8_t bit)
{
    g_layerMask ^= static_cast<std::uint8_t>((g_layerMask ^ static_cast<std::uint8_t>(~g_layerMask)) & bit);
    log::Step(
        "collision",
        "ToggleLayer bit=0x%02X mask=0x%02X",
        static_cast<unsigned>(bit),
        static_cast<unsigned>(g_layerMask));
    SetDebugDrawFlag(g_layerMask != 0);
}

void SetDebugDrawFlag(bool enabled)
{
    if (g_debugDrawFlag != nullptr)
    {
        const int before = *g_debugDrawFlag;
        const int after = enabled ? 1 : 0;
        if (before != after)
        {
            memlog::LogScalarWrite("debug_draw", trial::kVaDebugDrawFlag, "debug_draw", before, after, 4);
        }
        *g_debugDrawFlag = after;
    }
}

int GetLayerPaletteIndex(const std::uint8_t layerBit)
{
    return LayerPaletteForBit(layerBit);
}

void SetLayerPaletteIndex(const std::uint8_t layerBit, const int index)
{
    if (index < 0 || index > 255)
    {
        return;
    }

    switch (layerBit)
    {
    case kLayerHurt:
        g_paletteHurt = index;
        break;
    case kLayerHit:
        g_paletteHit = index;
        break;
    case kLayerCollision:
        g_paletteCollision = index;
        break;
    default:
        break;
    }
}

void DrawPaletteDebugPanel()
{
    if (!ImGui::CollapsingHeader("Hitbox palette (read-only)"))
    {
        return;
    }

    ImGui::TextWrapped(
        "Boxes use existing palette colors at the chosen indices — nothing is written to palette RAM. "
        "Pick indices whose RGB looks right in battle (avoid HP/UI colors). "
        "Copy a line below to report good indices.");

    int hurt = g_paletteHurt;
    int collision = g_paletteCollision;
    int hit = g_paletteHit;
    if (ImGui::InputInt("Hurt index (yellow)", &hurt))
    {
        SetLayerPaletteIndex(kLayerHurt, hurt);
    }
    if (ImGui::InputInt("Collision index (green)", &collision))
    {
        SetLayerPaletteIndex(kLayerCollision, collision);
    }
    if (ImGui::InputInt("Hit index (red)", &hit))
    {
        SetLayerPaletteIndex(kLayerHit, hit);
    }

    const std::uint8_t* palette = PaletteRam();
    if (palette != nullptr)
    {
        auto logRgb = [&](const char* label, const int idx)
        {
            if (idx < 0 || idx >= 256)
            {
                return;
            }
            const std::uint8_t* e = palette + static_cast<std::size_t>(idx) * 4u;
            ImGui::Text("%s [%d] = (%u, %u, %u)", label, idx, e[0], e[1], e[2]);
        };
        logRgb("Hurt", g_paletteHurt);
        logRgb("Collision", g_paletteCollision);
        logRgb("Hit", g_paletteHit);
        ImGui::TextDisabled("Native debug defaults: hurt=%d collision=%d hit=%d",
            trial::kPaletteHurt,
            trial::kPaletteCollision,
            trial::kPaletteHit);
    }

    if (ImGui::Button("Reset indices to native debug defaults"))
    {
        SetLayerPaletteIndex(kLayerHurt, trial::kPaletteHurt);
        SetLayerPaletteIndex(kLayerCollision, trial::kPaletteCollision);
        SetLayerPaletteIndex(kLayerHit, trial::kPaletteHit);
    }

    static int browseStart = 0;
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Browse from index", &browseStart);
    if (browseStart < 0)
    {
        browseStart = 0;
    }
    if (browseStart > 240)
    {
        browseStart = 240;
    }

    if (palette != nullptr && ImGui::BeginTable("palette_browse", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Idx");
        ImGui::TableSetupColumn("RGB");
        ImGui::TableSetupColumn("Swatch");
        ImGui::TableSetupColumn("Use");
        ImGui::TableHeadersRow();

        for (int i = browseStart; i < browseStart + 16 && i < 256; ++i)
        {
            const std::uint8_t* e = palette + static_cast<std::size_t>(i) * 4u;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("(%u, %u, %u)", e[0], e[1], e[2]);
            ImGui::TableSetColumnIndex(2);
            ImGui::ColorButton(
                "c",
                ImVec4(e[0] / 255.0f, e[1] / 255.0f, e[2] / 255.0f, 1.0f),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                ImVec2(40.0f, 16.0f));
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(i);
            if (ImGui::SmallButton("Hurt"))
            {
                SetLayerPaletteIndex(kLayerHurt, i);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Col"))
            {
                SetLayerPaletteIndex(kLayerCollision, i);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Hit"))
            {
                SetLayerPaletteIndex(kLayerHit, i);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DrawPlayerCollisionBoxes(int player, int camX, int camY, std::uint8_t layerMask)
{
    if (player == 0 || layerMask == 0 || !ResolveGameFunctions())
    {
        return;
    }

    unsigned char frameBlob[trial::kFrameBlobStride] = {};
    CopyFrameBlob(player, frameBlob);
    DrawCharacterLayer(player, frameBlob, camX, camY, layerMask);
    DrawProjectileRing(player, camX, camY, layerMask);
}

}
