#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

// 2001-04-08 EFZ trial (EFZ.EXE), image base 0x00400000.
namespace trial
{
constexpr std::uintptr_t kImageBase = 0x00400000u;

constexpr std::uintptr_t kVaDrawCollisionBoxes = 0x0044D680u;
constexpr std::uintptr_t kVaBattleTick = 0x0045CA20u;
constexpr std::uintptr_t kVaPresent = 0x00450210u;
// __thiscall (this, bufferIndex, flags); flags==1 loops.
constexpr std::uintptr_t kVaPlaySound = 0x00454D80u;
// (this, bufferIndex, soundId); no pointer check, so a freed buffer faults.
constexpr std::uintptr_t kVaSoundBufferSeek = 0x00455170u;
// (lpFileName, offset, dstA, count, dstB, mode); a missing file reaches operator new.
constexpr std::uintptr_t kVaAssetFileLoad = 0x0044B8F0u;
constexpr std::uintptr_t kVaOperatorNew = 0x00464BD0u;
// (rc1x4, surface, rc2x4, flag, mode); locks the surface without a null check.
constexpr std::uintptr_t kVaSurfaceBlit = 0x00450360u;
// Entry sanitizers must run even when the draw is skipped.
constexpr std::uintptr_t kVaBattleSceneComposite = 0x0044E430u;
constexpr std::uintptr_t kVaMenuTick = 0x0045E5E0u;
constexpr std::uintptr_t kVaCharSelectTick = 0x0045E9C0u;
constexpr std::uintptr_t kVaLoadingTick = 0x0045FC90u;
constexpr std::uintptr_t kVaTitleTick = 0x00462540u;
constexpr std::uintptr_t kVaPollPlayerInput = 0x00454410u;
constexpr std::uintptr_t kVaTransformEntity = 0x00459CE0u;
constexpr std::uintptr_t kVaTransformProjectile = 0x00459DF0u;
constexpr std::uintptr_t kVaDrawPaletteRect = 0x00453510u;

constexpr std::uintptr_t kVaDebugDrawFlag = 0x00CCDB80u;
constexpr std::uintptr_t kVaCameraX = 0x004B0B84u;
constexpr std::uintptr_t kVaCameraY = 0x004B0B80u;
constexpr std::uintptr_t kVaP1MoveId = 0x004B0BA0u;

constexpr std::uintptr_t kVaP1Player = 0x004B0B90u;
constexpr std::uintptr_t kVaP2Player = 0x008BF358u;
constexpr std::size_t kPlayerWordStride = 0x003FE7C8u;
// First WORD: low byte = character ID, high byte = palette ID.
constexpr std::size_t kOffSelectionCharacterId = 0x00u;
constexpr std::size_t kOffSelectionPaletteId = 0x01u;
constexpr std::uint8_t kMaxStockPaletteId = 5u; // char select wraps the selector modulo six

constexpr std::uintptr_t kVaFramePacingLastTick = 0x00CCDB2Cu;
constexpr std::uintptr_t kVaFramePacingToggle = 0x00CCDB64u;     // battle
constexpr std::uintptr_t kVaFramePacingToggleMenu = 0x00CCDB40u;
constexpr std::uintptr_t kVaFramePacingToggleCharSelect = 0x00CCDB20u;
constexpr std::uintptr_t kVaFramePacingToggleLoading = 0x00CCDB60u;
constexpr std::uintptr_t kVaFramePacingToggleTitle = 0x004A3EC4u;
constexpr std::uintptr_t kVaFramePacingBusyFlag = 0x00CCDB88u;
constexpr std::uintptr_t kVaNativeFpsSampleAnchor = 0x00CCDBB0u;
constexpr std::uintptr_t kVaNativeFpsLatch = 0x00CCDBB8u;
constexpr std::uintptr_t kVaNativeFpsCounter = 0x00CCDBACu;
constexpr std::uintptr_t kVaFrameSubsteps = 0x0047B658u;
// Player-pointer bound for subtick entity loops, not a gamespeed value.
constexpr std::uintptr_t kVaSubtickLoopBound = 0x00CCDB90u;
// When set, frontend ticks enter a hold loop (menu freeze path).
constexpr std::uintptr_t kVaSubstepHoldFlag = 0x00CCDBD8u;
constexpr std::uintptr_t kVaWinCount = 0x0047B580u;
constexpr std::uintptr_t kVaBattleScreenMode = 0x00CCDBA4u;

constexpr std::uintptr_t kVaBattleStateBegin = 0x004B0B80u;
constexpr std::uintptr_t kVaBattleStateEnd = 0x00CCDC80u;
constexpr std::size_t kBattleStateSize = kVaBattleStateEnd - kVaBattleStateBegin;

constexpr std::uintptr_t kVaBattleSimEnd = kVaP2Player + kPlayerWordStride;
constexpr std::size_t kBattleSimSize = kVaBattleSimEnd - kVaBattleStateBegin;

constexpr std::uintptr_t kVaMatchAuxBegin = kVaBattleSimEnd;
constexpr std::uintptr_t kVaMatchAuxEnd = kVaFramePacingLastTick;
constexpr std::size_t kMatchAuxSize = kVaMatchAuxEnd - kVaMatchAuxBegin;

// Never blind-restore; sanitized after load.
constexpr std::uintptr_t kVaScreenDispatchBegin = kVaFramePacingLastTick;
constexpr std::size_t kScreenDispatchSize = kVaBattleStateEnd - kVaScreenDispatchBegin;

// Page-granular; re-validate with savestate_profiler if a new character or move desyncs.
struct DynRegion
{
    std::uintptr_t va;
    std::size_t size;
};

constexpr DynRegion kDynamicBattleRegions[] = {
    {0x004B0B80u, 0x1000u}, // P1 core struct + near projectiles
    {0x004EBB80u, 0x1000u}, // P1 animation/frame working state
    {0x008BCB80u, 0x3000u}, // P1 projectile/AI tail + P2 core struct + hitstop globals
    {0x008F9B80u, 0x1000u}, // P2 animation/frame working state
    // Stop before the char-select pacing toggle, which differs between peers.
    {0x00CCBB80u, 0x1FA0u},
};

constexpr std::size_t kDynamicBattleSize =
    0x1000u + 0x1000u + 0x3000u + 0x1000u + 0x1FA0u;

constexpr int kDynamicRegionCount =
    static_cast<int>(sizeof(kDynamicBattleRegions) / sizeof(kDynamicBattleRegions[0]));

constexpr std::uint8_t kScreenMainMenu = 0u;
constexpr std::uint8_t kScreenBattle = 1u;
constexpr std::uint8_t kScreenCharSelect = 2u;
constexpr std::uint8_t kScreenLoading = 3u;    // loading, post-match, match init
constexpr std::uint8_t kScreenTitle = 4u;
constexpr int kPostMatchSubStateMin = 5;
constexpr int kPostMatchSubStateMax = 10;
constexpr int kWinScreenMoveIdA = 148;
constexpr int kWinScreenMoveIdB = 149;
constexpr std::uintptr_t kVaScreenSubState = 0x00CCDBC0u;
constexpr std::uintptr_t kVaScreenAnimCounter = 0x00CCDBBCu;
// Primary DirectDraw surface pointer.
constexpr std::uintptr_t kVaPrimarySurface = 0x004A20A0u;
// Magnitude double + two phase-angle dwords. Must be snapshotted.
constexpr std::uintptr_t kVaShakeState = 0x00CCDBC8u;
constexpr std::size_t kShakeStateSize = 0x10u;
// P1 then P2 back-charge edge flags; a stale flag starts the timer on one peer only.
constexpr std::uintptr_t kVaChargeEdgeFlags = 0x00CCDB78u;
constexpr std::size_t kChargeEdgeSize = 0x8u;
constexpr std::size_t kAuxShakeOffset = 0u;
constexpr std::size_t kAuxChargeEdgeOffset = kAuxShakeOffset + kShakeStateSize;
constexpr std::size_t kAuxScreenModeOffset = kAuxChargeEdgeOffset + kChargeEdgeSize;
constexpr std::size_t kAuxScreenSubStateOffset = kAuxScreenModeOffset + sizeof(std::uint8_t);
constexpr std::size_t kAuxScreenAnimCounterOffset = kAuxScreenSubStateOffset + sizeof(std::uint32_t);
constexpr std::size_t kAuxWinCountOffset = kAuxScreenAnimCounterOffset + sizeof(std::int32_t);
constexpr std::size_t kAuxWinnerSideOffset = kAuxWinCountOffset + sizeof(std::uint8_t);
constexpr std::size_t kAuxStateSize = kAuxWinnerSideOffset + sizeof(std::uint8_t);
// Its rand() calls must use the visual RNG stream, not the synced sim stream.
constexpr std::uintptr_t kVaEffectAnimUpdate = 0x00455790u;
constexpr std::uintptr_t kVaMainMenuCursor = 0x00CCDB34u; // 0=practice 1=? 2=VS Human
constexpr std::uintptr_t kVaVsHumanFlagP1 = 0x004B0BDCu;
constexpr std::uintptr_t kVaVsHumanFlagP2 = 0x008BF3A4u;

constexpr std::uint8_t kMainMenuVsHuman = 2u;
constexpr int kCharSelectSubVsHumanEntry = 4;
constexpr int kCharSelectSubInteractive = 0;
constexpr int kScreenSubMainMenuVsHuman = 1;
constexpr int kScreenSubTitleVsHuman = 2;
constexpr int kNativeTransitionNearComplete = 9; // anim counter; at 10 the game runs setup on the game thread
constexpr int kBattleIntroFirstFrameCounter = 0;
constexpr int kBattleIntroRawScreenFlipCounter = 306;
// 5/7/10 continue the same match; 6/8 are match wins that leave for title.
constexpr int kRoundContinueP1SubState = 5;
constexpr int kMatchWinP1SubState = 6;
constexpr int kRoundContinueP2SubState = 7;
constexpr int kMatchWinP2SubState = 8;
constexpr int kRoundContinueDrawSubState = 10;
constexpr std::uintptr_t kVaCharSelectP1State = 0x00CCDB30u;
constexpr std::uintptr_t kVaCharSelectP2State = 0x00CCDB31u;
constexpr std::uintptr_t kVaCharSelectP1Pick = 0x00CCDB48u;
constexpr std::uintptr_t kVaCharSelectP2Pick = 0x00CCDB4Cu;
constexpr std::uintptr_t kVaCharSelectSide = 0x00CCDB6Cu;
constexpr std::uintptr_t kVaCharSelectStage = 0x00CCDBA8u;
constexpr std::uintptr_t kVaSceneContext = 0x004A3EC8u;
constexpr std::uintptr_t kVaEffectSystem = 0x004A3F60u;
// Live effect slots end here; immutable frame-definition data follows.
constexpr std::size_t kEffectDynamicStateSize = 0x3580u;
constexpr std::uintptr_t kVaCharSelectTimerP1 = 0x004A3EB0u;
constexpr std::uintptr_t kVaCharSelectTimerP2 = 0x004A3EB4u;

constexpr std::uintptr_t kVaFnSceneReset = 0x00455720u;
constexpr std::uintptr_t kVaFnSceneEnter = 0x004555E0u;
constexpr std::uintptr_t kVaFnPlayerSlotInit = 0x00455530u;
constexpr std::uintptr_t kVaFnReleaseUiResources = 0x00454110u;
constexpr std::uintptr_t kVaFnCharSelectAssetInit = 0x00453C20u;

constexpr std::size_t kOffTrainingModeFlag = 0x04u;
constexpr std::size_t kOffPlayerTypeByte = 0x00u; // HIBYTE(word[player+0]); 0 = VS human
constexpr std::uintptr_t kVaDiActiveFlag = 0x004A3F5Cu;
constexpr std::uintptr_t kVaKeyboardDevice = 0x004A3E80u;
// key.ini device type: 0=keyboard 1=joy1 2=joy2; DIK indices at kVaDiKeyCodesP1/P2.
constexpr std::uintptr_t kVaDiDeviceTypesP1 = 0x004A3E68u;
constexpr std::uintptr_t kVaDiDeviceTypesP2 = 0x004A3E70u;
constexpr std::uintptr_t kVaDiKeyCodesP1 = 0x0047B290u;
constexpr std::uintptr_t kVaDiKeyCodesP2 = 0x0047B298u;

constexpr std::size_t kOffMoveId = 16u;
constexpr std::size_t kOffHp = 0x14u;
constexpr std::size_t kOffEForce = 0x20u;
constexpr std::size_t kOffSpStock = 0x48u;
// World-space horizontal position (double).
constexpr std::size_t kOffPositionX = 0x70u;
constexpr std::size_t kOffFacing = 0x80u;
constexpr std::size_t kOffCpuControlFlag = 0x4Cu;
constexpr std::size_t kOffInputHoriz = 0x40CE06u;
constexpr std::size_t kOffInputVertical = 0x40CE07u;
constexpr std::size_t kOffInputButtonA = 0x40CE08u;
constexpr std::size_t kOffInputButtonB = 0x40CE09u;
constexpr std::size_t kOffInputButtonC = 0x40CE0Au;
constexpr std::size_t kOffInputButtonD = 0x40CE0Bu;
constexpr std::size_t kOffInputButtonALatch = 0x40CE0Cu;
constexpr std::size_t kOffInputButtonBLatch = 0x40CE10u;
constexpr std::size_t kOffInputButtonCLatch = 0x40CE14u;
constexpr std::size_t kOffInputButtonDLatch = 0x40CE18u;
constexpr std::size_t kOffInputHistoryIndex = 0x40CED8u;
constexpr std::size_t kOffInputHistoryBase = 0x40CE24u;
constexpr std::size_t kOffSubframe = 242950u;
constexpr std::size_t kOffFrameBlobBase = 242980u;
constexpr std::size_t kFrameBlobStride = 200u;
constexpr std::size_t kMoveBlobStride = 8008u;

constexpr std::size_t kOffProjectileRingHead = 4247260u;
constexpr std::size_t kOffProjectileAlive = 4247264u;
constexpr std::size_t kOffProjectileMoveId = 4248044u;
constexpr std::size_t kOffProjectileSubframe = 4248164u;
constexpr int kProjectileRingSize = 60;

constexpr std::size_t kHurtRectBase = 0u;
constexpr std::size_t kHitRectBase = 80u;
constexpr std::size_t kCollisionRectBase = 144u;

constexpr int kHurtRectCount = 5;
constexpr int kHitRectCount = 4;
constexpr int kCollisionRectCount = 1;

// Fallback palette indices; a runtime scan picks the nearest.
constexpr int kPaletteHurt = 219;       // yellow
constexpr int kPaletteHit = 221;        // red
constexpr int kPaletteCollision = 237;  // green

constexpr std::uintptr_t kVaPaletteEntries = 0x0049F440u;     // PALETTEENTRY[256+...]
constexpr std::uintptr_t kVaDirectDrawPalette = 0x0047F2E4u;
constexpr std::uintptr_t kVaScreenEventQueue = 0x004A73EEu;
constexpr std::size_t kScreenEventQueueSize = 240u;

inline std::uintptr_t VaToRva(std::uintptr_t va)
{
    return va - kImageBase;
}

inline void* ResolveVa(std::uintptr_t va)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (base == 0u)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(base + VaToRva(va));
}

}
