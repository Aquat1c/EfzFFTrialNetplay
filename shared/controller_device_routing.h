#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace trialnet::input
{

using ControllerDeviceId = std::uint32_t;
inline constexpr ControllerDeviceId kNoControllerDevice = 0u;

struct ControllerDeviceCandidate
{
    ControllerDeviceId id = kNoControllerDevice;
    bool mapped = false;
};

struct ControllerDeviceAssignment
{
    std::array<ControllerDeviceId, 2> player = {
        kNoControllerDevice,
        kNoControllerDevice,
    };
};

inline std::vector<int> ControllerPlayerOrder(
    const bool p1UsesController,
    const bool p2UsesController)
{
    std::vector<int> players;
    players.reserve(2u);
    if (p1UsesController)
    {
        players.push_back(0);
    }
    if (p2UsesController)
    {
        players.push_back(1);
    }
    return players;
}

// A mapped gamepad and its raw interface share one instance ID; keep each ID once.
inline std::vector<ControllerDeviceId> PreferredControllerDeviceOrder(
    const std::vector<ControllerDeviceCandidate>& candidates)
{
    std::vector<ControllerDeviceId> ordered;
    ordered.reserve(candidates.size());

    const auto appendGroup = [&](const bool mapped)
    {
        for (const ControllerDeviceCandidate& candidate : candidates)
        {
            if (candidate.id == kNoControllerDevice || candidate.mapped != mapped)
            {
                continue;
            }

            bool duplicate = false;
            for (const ControllerDeviceId existing : ordered)
            {
                if (existing == candidate.id)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                ordered.push_back(candidate.id);
            }
        }
    };

    appendGroup(true);
    appendGroup(false);
    return ordered;
}

// Independent of gameplay side: online always samples the launcher's P1 bank.
inline ControllerDeviceAssignment AssignControllerDevices(
    const std::vector<ControllerDeviceCandidate>& candidates,
    const bool p1UsesController,
    const bool p2UsesController)
{
    ControllerDeviceAssignment assignment{};
    const std::vector<ControllerDeviceId> ordered = PreferredControllerDeviceOrder(candidates);
    const std::vector<int> players = ControllerPlayerOrder(p1UsesController, p2UsesController);
    const std::size_t count = players.size() < ordered.size() ? players.size() : ordered.size();
    for (std::size_t i = 0u; i < count; ++i)
    {
        assignment.player[static_cast<std::size_t>(players[i])] = ordered[i];
    }
    return assignment;
}

}
