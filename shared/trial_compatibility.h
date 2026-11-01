#pragma once

#include <cstdint>

namespace trialnet::compat
{

// Bump when handshake, session file, wire format, simulation or NAT behavior breaks.
constexpr std::uint16_t kLauncherProtocolMajor = 1u;
constexpr std::uint16_t kCompatibilityVersion = 14u;
constexpr std::uint32_t kCompatibilityId =
    (static_cast<std::uint32_t>(kLauncherProtocolMajor) << 16u)
    | static_cast<std::uint32_t>(kCompatibilityVersion);

static_assert(kCompatibilityVersion <= 0xFFu, "DLL compatibility byte overflow");

}
