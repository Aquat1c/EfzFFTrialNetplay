#pragma once

#include "input_binding_tokens.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace trialnet::input
{

inline constexpr std::size_t kOfflineFightSlotCount = 8u;
inline constexpr std::uint8_t kDiKeyboardDevice = 0u;
inline constexpr std::uint8_t kDiDisabledDevice = 0xFFu;
inline constexpr std::uint8_t kDiKeyDown = 0x80u;

struct OfflineDiBindingPlan
{
    std::array<std::uint8_t, kOfflineFightSlotCount> deviceTypes{};
    std::array<std::uint8_t, kOfflineFightSlotCount> keyCodes{};
};

// Active slots get a private byte, inactive are disabled: no native joystick OR-in.
inline OfflineDiBindingPlan BuildOfflineDiBindingPlan(
    const int playerIndex,
    const Source source,
    const std::array<std::uint8_t, kOfflineFightSlotCount>& keyboardDiks,
    const std::array<bool, kOfflineFightSlotCount>& controllerBindingsActive)
{
    OfflineDiBindingPlan plan{};
    plan.deviceTypes.fill(kDiDisabledDevice);
    if (playerIndex < 0 || playerIndex > 1)
    {
        return plan;
    }

    // Unassigned DI scan codes no SDL scancode translates to, so P1/P2 cannot collide.
    static constexpr std::array<std::array<std::uint8_t, kOfflineFightSlotCount>, 2>
        kControllerDiks = {{
            {{0x59u, 0x5Au, 0x5Bu, 0x5Cu, 0x5Du, 0x5Eu, 0x5Fu, 0x60u}},
            {{0x61u, 0x62u, 0x63u, 0x67u, 0x68u, 0x69u, 0x6Au, 0x6Bu}},
        }};

    for (std::size_t slot = 0u; slot < kOfflineFightSlotCount; ++slot)
    {
        const std::uint8_t dik = source == Source::Controller
            ? (controllerBindingsActive[slot]
                ? kControllerDiks[static_cast<std::size_t>(playerIndex)][slot]
                : 0u)
            : keyboardDiks[slot];
        plan.deviceTypes[slot] = dik != 0u ? kDiKeyboardDevice : kDiDisabledDevice;
        plan.keyCodes[slot] = dik;
    }
    return plan;
}

template <typename KeyboardSampler, typename ControllerSampler>
bool SampleConfiguredFightSource(
    const Source source,
    KeyboardSampler&& keyboardSampler,
    ControllerSampler&& controllerSampler)
{
    if (source == Source::Controller)
    {
        return std::forward<ControllerSampler>(controllerSampler)();
    }
    return std::forward<KeyboardSampler>(keyboardSampler)();
}

// Offline controller ownership must not depend on the online local-side remap.
inline bool ControllerPlayerMaySample(
    const int playerIndex,
    const bool netplayActive,
    const int onlineLocalBindingPlayer)
{
    if (playerIndex < 0 || playerIndex > 1)
    {
        return false;
    }
    return !netplayActive
        || (onlineLocalBindingPlayer >= 0 && onlineLocalBindingPlayer <= 1
            && playerIndex == onlineLocalBindingPlayer);
}

inline void ClearManagedDiSlots(
    std::uint8_t* const out256,
    const std::uint8_t* const deviceTypes,
    const std::uint8_t* const keyCodes,
    const std::size_t slotCount)
{
    if (out256 == nullptr || deviceTypes == nullptr || keyCodes == nullptr)
    {
        return;
    }
    for (std::size_t slot = 0u; slot < slotCount; ++slot)
    {
        const std::uint8_t dik = keyCodes[slot];
        if (deviceTypes[slot] == kDiKeyboardDevice && dik != 0u)
        {
            out256[dik] = 0u;
        }
    }
}

inline void MergeManagedDiSlots(
    std::uint8_t* const out256,
    const std::uint8_t* const deviceTypes,
    const std::uint8_t* const keyCodes,
    const bool* const slotDown,
    const std::size_t slotCount)
{
    if (out256 == nullptr || deviceTypes == nullptr || keyCodes == nullptr
        || slotDown == nullptr)
    {
        return;
    }
    for (std::size_t slot = 0u; slot < slotCount; ++slot)
    {
        const std::uint8_t dik = keyCodes[slot];
        if (deviceTypes[slot] == kDiKeyboardDevice && dik != 0u && slotDown[slot])
        {
            out256[dik] = kDiKeyDown;
        }
    }
}

// The runtime must not resample between logging this value and injecting it.
inline void MergeManagedDiMask(
    std::uint8_t* const out256,
    const std::uint8_t* const deviceTypes,
    const std::uint8_t* const keyCodes,
    const std::uint8_t wireMask,
    const std::size_t slotCount)
{
    std::array<bool, kOfflineFightSlotCount> slotDown{};
    const std::size_t count = slotCount < slotDown.size() ? slotCount : slotDown.size();
    for (std::size_t slot = 0u; slot < count; ++slot)
    {
        slotDown[slot] = (wireMask & static_cast<std::uint8_t>(1u << slot)) != 0u;
    }
    MergeManagedDiSlots(out256, deviceTypes, keyCodes, slotDown.data(), count);
}

}
