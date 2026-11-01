#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace trialnet::autopunch
{

constexpr char kCoordinatorHost[] = "delthas.fr";
// Pinned: Poll() must never block a tick in DNS. Re-verify when releasing.
constexpr char kCoordinatorIpv4[] = "65.108.63.185";
constexpr std::uint16_t kCoordinatorPort = 14763u;

constexpr std::uint32_t kDirectAttemptMs = 1500u;
constexpr std::uint32_t kConnectTimeoutMs = 12000u;
constexpr std::uint32_t kRegisterIntervalMs = 500u;
constexpr std::uint32_t kLookupIntervalMs = 500u;
constexpr std::uint32_t kDirectPunchIntervalMs = 125u;
constexpr std::uint32_t kResolveRetryMs = 2000u;

using RegisterPacket = std::array<std::uint8_t, 2>;
using LookupPacket = std::array<std::uint8_t, 8>;

struct Mapping
{
    std::uint16_t internalPort = 0;
    std::uint16_t natPort = 0;
    // IPv4 bytes in network order, matching sockaddr_in::sin_addr.s_addr.
    std::uint32_t hostNetworkOrder = 0;
};

constexpr void WritePort(std::uint8_t* out, const std::uint16_t port)
{
    out[0] = static_cast<std::uint8_t>(port >> 8u);
    out[1] = static_cast<std::uint8_t>(port & 0xFFu);
}

constexpr std::uint16_t ReadPort(const std::uint8_t* in)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(in[0]) << 8u) | static_cast<std::uint16_t>(in[1]));
}

inline RegisterPacket MakeRegister(const std::uint16_t localPort)
{
    RegisterPacket packet{};
    WritePort(packet.data(), localPort);
    return packet;
}

inline LookupPacket MakeLookup(
    const std::uint16_t localPort,
    const std::uint32_t targetHostNetworkOrder,
    const std::uint16_t targetInternalPort)
{
    LookupPacket packet{};
    WritePort(packet.data(), localPort);
    std::memcpy(packet.data() + 2u, &targetHostNetworkOrder, sizeof(targetHostNetworkOrder));
    WritePort(packet.data() + 6u, targetInternalPort);
    return packet;
}

inline bool ParseMapping(const void* bytes, const std::size_t length, Mapping* out)
{
    if (bytes == nullptr || length != LookupPacket{}.size() || out == nullptr)
    {
        return false;
    }

    const auto* const data = static_cast<const std::uint8_t*>(bytes);
    Mapping mapping{};
    mapping.internalPort = ReadPort(data);
    mapping.natPort = ReadPort(data + 2u);
    std::memcpy(&mapping.hostNetworkOrder, data + 4u, sizeof(mapping.hostNetworkOrder));
    if (mapping.internalPort == 0u || mapping.natPort == 0u || mapping.hostNetworkOrder == 0u)
    {
        return false;
    }
    *out = mapping;
    return true;
}

inline bool IsDirectPunch(const void* bytes, const std::size_t length)
{
    return bytes != nullptr && length == 1u && *static_cast<const std::uint8_t*>(bytes) == 0u;
}

}
