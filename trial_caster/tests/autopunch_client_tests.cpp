#include "autopunch_client.h"
#include "autopunch_protocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace
{

using trialnet::autopunch::Client;
using trialnet::autopunch::InboundResult;

struct SocketPair
{
    SOCKET sender = INVALID_SOCKET;
    SOCKET receiver = INVALID_SOCKET;
    std::uint16_t senderPort = 0;
    std::uint16_t receiverPort = 0;

    ~SocketPair()
    {
        if (sender != INVALID_SOCKET)
        {
            closesocket(sender);
        }
        if (receiver != INVALID_SOCKET)
        {
            closesocket(receiver);
        }
    }
};

bool SameEndpoint(const sockaddr_in& lhs, const sockaddr_in& rhs)
{
    return lhs.sin_family == rhs.sin_family
        && lhs.sin_addr.s_addr == rhs.sin_addr.s_addr
        && lhs.sin_port == rhs.sin_port;
}

bool BindUdp(SOCKET* const outSocket, std::uint16_t* const outPort)
{
    if (outSocket == nullptr || outPort == nullptr)
    {
        return false;
    }
    const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        closesocket(handle);
        return false;
    }

    int localLength = sizeof(local);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&local), &localLength) == SOCKET_ERROR
        || local.sin_port == 0u)
    {
        closesocket(handle);
        return false;
    }
    *outSocket = handle;
    *outPort = ntohs(local.sin_port);
    return true;
}

bool MakeSockets(SocketPair* const sockets)
{
    return sockets != nullptr
        && BindUdp(&sockets->sender, &sockets->senderPort)
        && BindUdp(&sockets->receiver, &sockets->receiverPort);
}

sockaddr_in MakeAddress(const char* const ipv4, const std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    InetPtonA(AF_INET, ipv4, &address.sin_addr);
    return address;
}

sockaddr_in CoordinatorAddress()
{
    return MakeAddress(
        trialnet::autopunch::kCoordinatorIpv4,
        trialnet::autopunch::kCoordinatorPort);
}

std::array<std::uint8_t, 8> MakeMapping(
    const std::uint16_t internalPort,
    const std::uint16_t natPort,
    const sockaddr_in& peer)
{
    std::array<std::uint8_t, 8> bytes{};
    trialnet::autopunch::WritePort(bytes.data(), internalPort);
    trialnet::autopunch::WritePort(bytes.data() + 2u, natPort);
    std::memcpy(bytes.data() + 4u, &peer.sin_addr.s_addr, sizeof(peer.sin_addr.s_addr));
    return bytes;
}

bool ReceivePunches(const SOCKET receiver, const unsigned expected)
{
    unsigned received = 0;
    const DWORD deadline = GetTickCount() + 1000u;
    while (received < expected && static_cast<LONG>(GetTickCount() - deadline) < 0)
    {
        fd_set readSet{};
        FD_ZERO(&readSet);
        FD_SET(receiver, &readSet);
        timeval timeout{};
        timeout.tv_usec = 50000;
        if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0)
        {
            continue;
        }
        std::uint8_t byte = 0xFFu;
        const int bytes = recvfrom(
            receiver,
            reinterpret_cast<char*>(&byte),
            sizeof(byte),
            0,
            nullptr,
            nullptr);
        if (bytes == 1 && byte == 0u)
        {
            ++received;
        }
    }
    return received == expected;
}

bool VerifyPassiveCoordinatorMapping()
{
    SocketPair sockets{};
    if (!MakeSockets(&sockets))
    {
        return false;
    }

    Client client{};
    if (!client.Attach(sockets.sender, sockets.senderPort, nullptr, false))
    {
        return false;
    }
    client.Start(1u);

    const std::uint8_t punch = 0u;
    const sockaddr_in arbitraryPeer = MakeAddress("198.51.100.9", 41000u);
    sockaddr_in untouched = MakeAddress("203.0.113.7", 42000u);
    const sockaddr_in sentinel = untouched;
    if (client.HandleInbound(&punch, sizeof(punch), arbitraryPeer, &untouched)
            != InboundResult::Consumed
        || !SameEndpoint(untouched, sentinel)
        || client.WasAssisted())
    {
        return false;
    }

    const sockaddr_in mappedAddress = MakeAddress("127.0.0.1", sockets.receiverPort);
    const auto mapping = MakeMapping(49152u, sockets.receiverPort, mappedAddress);

    sockaddr_in wrongCoordinator = CoordinatorAddress();
    wrongCoordinator.sin_port = htons(
        static_cast<std::uint16_t>(trialnet::autopunch::kCoordinatorPort + 1u));
    untouched = sentinel;
    if (client.HandleInbound(mapping.data(), mapping.size(), wrongCoordinator, &untouched)
            != InboundResult::NotHandled
        || !SameEndpoint(untouched, sentinel)
        || client.GetStats().mappingsReceived != 0u
        || client.WasAssisted())
    {
        return false;
    }

    sockaddr_in learned{};
    if (client.HandleInbound(
            mapping.data(),
            mapping.size(),
            CoordinatorAddress(),
            &learned) != InboundResult::PeerMapped
        || !SameEndpoint(learned, mappedAddress)
        || !client.WasAssisted()
        || client.GetStats().mappingsReceived != 1u
        || client.GetStats().directPunchesSent != 4u
        || !ReceivePunches(sockets.receiver, 4u))
    {
        return false;
    }

    const sockaddr_in secondAddress = MakeAddress("127.0.0.2", 43000u);
    const auto secondMapping = MakeMapping(49153u, 43000u, secondAddress);
    learned = {};
    if (client.HandleInbound(
            secondMapping.data(),
            secondMapping.size(),
            CoordinatorAddress(),
            &learned) != InboundResult::PeerMapped
        || !SameEndpoint(learned, secondAddress)
        || client.GetStats().mappingsReceived != 2u)
    {
        return false;
    }

    const auto punchesBeforeService = client.GetStats().directPunchesSent;
    client.Service(1000u);
    return client.GetStats().lookupsSent == 0u
        && client.GetStats().directPunchesSent == punchesBeforeService;
}

bool VerifyConfiguredTargetBehavior()
{
    SocketPair sockets{};
    SOCKET natReceiver = INVALID_SOCKET;
    std::uint16_t natPort = 0;
    if (!MakeSockets(&sockets) || !BindUdp(&natReceiver, &natPort))
    {
        if (natReceiver != INVALID_SOCKET)
        {
            closesocket(natReceiver);
        }
        return false;
    }

    const sockaddr_in configured = MakeAddress("127.0.0.1", sockets.receiverPort);
    Client client{};
    if (!client.Attach(sockets.sender, sockets.senderPort, &configured, true))
    {
        closesocket(natReceiver);
        return false;
    }
    client.Start(1u);

    const sockaddr_in otherHost = MakeAddress("127.0.0.2", natPort);
    const auto mismatchedHost = MakeMapping(sockets.receiverPort, natPort, otherHost);
    if (client.HandleInbound(
            mismatchedHost.data(),
            mismatchedHost.size(),
            CoordinatorAddress(),
            nullptr) != InboundResult::Consumed
        || client.GetStats().mappingsReceived != 0u)
    {
        closesocket(natReceiver);
        return false;
    }

    const auto mismatchedPort = MakeMapping(
        static_cast<std::uint16_t>(sockets.receiverPort + 1u),
        natPort,
        configured);
    if (client.HandleInbound(
            mismatchedPort.data(),
            mismatchedPort.size(),
            CoordinatorAddress(),
            nullptr) != InboundResult::Consumed
        || client.GetStats().mappingsReceived != 0u)
    {
        closesocket(natReceiver);
        return false;
    }

    const sockaddr_in mapped = MakeAddress("127.0.0.1", natPort);
    const auto valid = MakeMapping(sockets.receiverPort, natPort, mapped);
    sockaddr_in learned{};
    if (client.HandleInbound(valid.data(), valid.size(), CoordinatorAddress(), &learned)
            != InboundResult::PeerMapped
        || !SameEndpoint(learned, mapped)
        || client.GetStats().mappingsReceived != 1u)
    {
        closesocket(natReceiver);
        return false;
    }

    const std::uint8_t punch = 0u;
    sockaddr_in observed{};
    if (client.HandleInbound(&punch, sizeof(punch), mapped, &observed)
            != InboundResult::PeerObserved
        || !SameEndpoint(observed, mapped))
    {
        closesocket(natReceiver);
        return false;
    }
    observed = {};
    if (client.HandleInbound(&punch, sizeof(punch), otherHost, &observed)
            != InboundResult::NotHandled
        || observed.sin_family != 0)
    {
        closesocket(natReceiver);
        return false;
    }

    closesocket(natReceiver);
    // A lookup may not route to the public coordinator on every CI host.
    return client.GetStats().directPunchesSent >= 5u
        && client.GetStats().directPunchesReceived == 1u;
}

}

int main()
{
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
    {
        return 1;
    }

    const bool passive = VerifyPassiveCoordinatorMapping();
    const bool active = VerifyConfiguredTargetBehavior();
    WSACleanup();
    if (!passive)
    {
        return 2;
    }
    if (!active)
    {
        return 3;
    }
    return 0;
}
