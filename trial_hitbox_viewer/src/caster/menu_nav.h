#pragma once

#include <cstdint>

namespace thv::menu
{

// Capture and DirectInput injection must use these same wire bits.
inline constexpr std::uint8_t kSystemStateEsc = 0x01u;
inline constexpr std::uint8_t kSystemStateIncFsf = 0x02u;
inline constexpr std::uint8_t kSystemStateDecFsf = 0x04u;
inline constexpr std::uint8_t kSystemStateF4 = 0x08u;
inline constexpr std::uint8_t kSystemStateF5 = 0x10u;
inline constexpr std::uint8_t kSystemStateF6 = 0x20u;
inline constexpr std::uint8_t kSystemStateF7 = 0x40u;
inline constexpr std::uint8_t kSystemStateF8 = 0x80u;

// The wire carries only ESC and negotiated F4/F5.
inline constexpr std::uint8_t kOnlinePracticeSystemMask =
    kSystemStateF4 | kSystemStateF5;
inline constexpr std::uint8_t kOnlineKnownSystemMask =
    kSystemStateEsc | kOnlinePracticeSystemMask;

constexpr std::uint8_t AllowedOnlineSystemMaskForPolicy(const bool allowPracticeKeys)
{
    return allowPracticeKeys ? kOnlineKnownSystemMask : kSystemStateEsc;
}

constexpr std::uint8_t FilterOnlineSystemStateForPolicy(
    const std::uint8_t state,
    const bool allowPracticeKeys)
{
    return static_cast<std::uint8_t>(
        state & AllowedOnlineSystemMaskForPolicy(allowPracticeKeys));
}

enum class NavScreen : std::uint8_t
{
    None = 0,
    MainMenu,
    Title,
    CharSelect,
    Loading,
    Battle,
    Post,
};

NavScreen DetectNavScreen();
const char* NavScreenName(NavScreen screen);

bool IsSyncEligible(NavScreen screen);
bool IsSyncEligibleNow();

bool IsFightInputAllowed(NavScreen screen);
bool IsFightInputAllowedNow();

bool UsesLockstepWireDiOverlay(NavScreen screen);

std::uint8_t SampleLocalSystemState();

void SetOnlinePracticeKeysAllowed(bool allowed);
bool OnlinePracticeKeysAllowed();
std::uint8_t AllowedOnlineSystemMask();
// Apply at capture and at remote ingestion.
std::uint8_t FilterOnlineSystemState(std::uint8_t state);
// Spectator records may carry negotiated F4/F5; unknown bits are rejected.
std::uint8_t FilterRecordedSystemState(std::uint8_t state);
bool ApplyTrialPracticeSystemState(std::uint8_t state);

}
