#include "trial_input_codec.h"

#include "caster_input.h"
#include "sdl_input.h"
#include "trial_addresses.h"

#include <cstring>

namespace thv::codec
{
namespace
{

bool SlotDownForPlayer(const int playerIndex, const int slot)
{
    return input::IsConfiguredSlotDownForPlayer(playerIndex, slot);
}

bool SlotDown(int slot)
{
    return SlotDownForPlayer(caster::GetLocalSide(), slot);
}

std::uint16_t PackHistoryWord(const std::uint8_t* player)
{
    constexpr std::uint8_t kNeg = 0xFF;
    const std::uint8_t horiz = player[trial::kOffInputHoriz];
    const std::uint8_t vert = player[trial::kOffInputVertical];
    const std::uint8_t btnA = player[trial::kOffInputButtonA];
    const std::uint8_t btnB = player[trial::kOffInputButtonB];
    const std::uint8_t btnC = player[trial::kOffInputButtonC];
    const std::uint8_t btnD = player[trial::kOffInputButtonD];

    std::uint16_t packed = 0;
    if (horiz == 1u)
    {
        packed |= 1u;
    }
    if (horiz == kNeg)
    {
        packed |= 2u;
    }
    if (vert == 1u)
    {
        packed |= 4u;
    }
    if (vert == kNeg)
    {
        packed |= 8u;
    }
    if (btnA == 1u)
    {
        packed |= 16u;
    }
    if (btnB == 1u)
    {
        packed |= 32u;
    }
    if (btnC == 1u)
    {
        packed |= 64u;
    }
    if (btnD == 1u)
    {
        packed |= 128u;
    }
    return packed;
}

void AppendInputHistory(std::uint8_t* player)
{
    const std::uint16_t packed = PackHistoryWord(player);
    std::uint16_t& historyIndex =
        *reinterpret_cast<std::uint16_t*>(player + trial::kOffInputHistoryIndex);
    *reinterpret_cast<std::uint16_t*>(
        player + trial::kOffInputHistoryBase + static_cast<std::size_t>(historyIndex) * sizeof(std::uint16_t)) =
        packed;

    const std::int16_t moveId = *reinterpret_cast<const std::int16_t*>(player + trial::kOffMoveId);
    if ((packed & 0xF0u) != 0u && moveId >= 200)
    {
        *reinterpret_cast<std::uint16_t*>(player + trial::kOffInputHistoryBase - 96) = packed;
    }
    if ((packed & 0xF0u) != 0u && moveId >= 163 && moveId <= 166)
    {
        *reinterpret_cast<std::uint16_t*>(player + trial::kOffInputHistoryBase - 94) = packed;
        player[trial::kOffInputHistoryBase - 92] = 15u;
    }

    ++historyIndex;
    if (historyIndex >= 90u)
    {
        historyIndex = 0u;
    }
}

}

std::uint8_t PackWireMask(const std::uint32_t slots[kSlotCount])
{
    std::uint8_t mask = 0;
    for (int i = 0; i < kSlotCount; ++i)
    {
        if (slots[i] != 0u)
        {
            mask |= static_cast<std::uint8_t>(1u << i);
        }
    }
    return mask;
}

void UnpackWireMask(std::uint8_t mask, std::uint32_t slots[kSlotCount])
{
    for (int i = 0; i < kSlotCount; ++i)
    {
        slots[i] = (mask & static_cast<std::uint8_t>(1u << i)) != 0u ? 1u : 0u;
    }
}

std::uint8_t PackWireMaskFromPlayer(const void* playerBase)
{
    if (playerBase == nullptr)
    {
        return 0;
    }

    const auto* const bytes = static_cast<const std::uint8_t*>(playerBase);
    const std::uint8_t horiz = bytes[trial::kOffInputHoriz];
    const std::uint8_t vert = bytes[trial::kOffInputVertical];
    const bool faceRight = bytes[trial::kOffFacing] > 0;

    std::uint8_t mask = 0;
    if (vert == 0xFFu)
    {
        mask |= 1u << 0;
    }
    if (vert == 1u)
    {
        mask |= 1u << 1;
    }

    // horiz is an absolute screen direction: 0xFF=Left, 1=Right; inverse ignores facing.
    (void)faceRight;
    if (horiz == 0xFFu)
    {
        mask |= 1u << 2;
    }
    if (horiz == 1u)
    {
        mask |= 1u << 3;
    }

    if (bytes[trial::kOffInputButtonA] == 1u)
    {
        mask |= 1u << 4;
    }
    if (bytes[trial::kOffInputButtonB] == 1u)
    {
        mask |= 1u << 5;
    }
    if (bytes[trial::kOffInputButtonC] == 1u)
    {
        mask |= 1u << 6;
    }
    if (bytes[trial::kOffInputButtonD] == 1u)
    {
        mask |= 1u << 7;
    }

    return mask;
}

std::uint8_t SampleSdlWireMaskForPlayer(const int playerIndex)
{
    std::uint32_t slots[kSlotCount] = {};
    for (int i = 0; i < kSlotCount; ++i)
    {
        slots[i] = SlotDownForPlayer(playerIndex, i) ? 1u : 0u;
    }
    return PackWireMask(slots);
}

std::uint8_t SampleSdlWireMask()
{
    return SampleSdlWireMaskForPlayer(caster::LocalBindingPlayerIndex());
}

void ApplyWireMaskToPlayer(void* playerBase, std::uint8_t wireMask)
{
    if (playerBase == nullptr)
    {
        return;
    }

    auto* const player = static_cast<std::uint8_t*>(playerBase);
    std::uint32_t slots[kSlotCount] = {};
    UnpackWireMask(wireMask, slots);

    player[trial::kOffInputHoriz] = 0;
    player[trial::kOffInputVertical] = 0;
    player[trial::kOffInputButtonA] = 0;
    player[trial::kOffInputButtonB] = 0;
    player[trial::kOffInputButtonC] = 0;
    player[trial::kOffInputButtonD] = 0;

    if (slots[0] != 0u)
    {
        player[trial::kOffInputVertical] = 0xFFu;
    }
    else if (slots[1] != 0u)
    {
        player[trial::kOffInputVertical] = 1u;
    }

    const bool faceRight = player[trial::kOffFacing] > 0;
    if (faceRight)
    {
        if (slots[2] != 0u)
        {
            player[trial::kOffInputHoriz] = 0xFFu;
        }
        else if (slots[3] != 0u)
        {
            player[trial::kOffInputHoriz] = 1u;
        }
    }
    else
    {
        if (slots[3] != 0u)
        {
            player[trial::kOffInputHoriz] = 1u;
        }
        else if (slots[2] != 0u)
        {
            player[trial::kOffInputHoriz] = 0xFFu;
        }
    }

    auto applyButton = [&](int slot, std::size_t buttonOff, std::size_t latchOff)
    {
        if (slots[slot] == 0u)
        {
            *reinterpret_cast<std::uint32_t*>(player + latchOff) = 0u;
            return;
        }

        const std::uint32_t previous = *reinterpret_cast<std::uint32_t*>(player + latchOff);
        *reinterpret_cast<std::uint32_t*>(player + latchOff) = 1u;
        player[buttonOff] = previous == 0u ? 1u : 0u;
    };

    applyButton(4, trial::kOffInputButtonA, trial::kOffInputButtonALatch);
    applyButton(5, trial::kOffInputButtonB, trial::kOffInputButtonBLatch);
    applyButton(6, trial::kOffInputButtonC, trial::kOffInputButtonCLatch);
    applyButton(7, trial::kOffInputButtonD, trial::kOffInputButtonDLatch);

    AppendInputHistory(player);
}

}
