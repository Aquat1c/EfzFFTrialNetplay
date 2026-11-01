#include "ip_info.h"

#include "session_log.h"
#include "util.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace trial_caster::ipinfo
{
namespace
{

enum class PublicLookupResult : int
{
    Pending = 0,
    WinInet = 1,
    Winsock = 2,
    Unavailable = 3,
};

struct DetectionState
{
    std::mutex mutex;
    std::string lan;
    std::string publicIp;
    std::atomic<bool> publicPending{false};
    std::atomic<bool> started{false};
    std::atomic<bool> outcomeLogged{false};
    std::atomic<PublicLookupResult> outcome{PublicLookupResult::Pending};
};

DetectionState& State()
{
    // Process lifetime: a detached worker must not touch destructed CRT objects.
    static DetectionState* const state = new DetectionState();
    return *state;
}

std::string DetectLan()
{
    char host[256] = {};
    if (gethostname(host, sizeof(host)) != 0)
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_ACP, 0, host, -1, nullptr, 0);
    if (required <= 1)
    {
        return {};
    }
    std::wstring wideHost(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, 0, host, -1, wideHost.data(), required) <= 0)
    {
        return {};
    }
    wideHost.resize(static_cast<std::size_t>(required - 1));
    const std::wstring resolved = util::ResolveConnectHostIpv4(wideHost);
    if (resolved.empty())
    {
        return {};
    }
    const int utf8Required = WideCharToMultiByte(
        CP_UTF8, 0, resolved.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Required <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(utf8Required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        resolved.c_str(),
        -1,
        result.data(),
        utf8Required,
        nullptr,
        nullptr);
    result.resize(static_cast<std::size_t>(utf8Required - 1));
    return result;
}

std::string NormalizePublicIpv4(std::string text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1u);

    IN_ADDR address{};
    if (InetPtonA(AF_INET, text.c_str(), &address) != 1)
    {
        return {};
    }

    char canonical[INET_ADDRSTRLEN] = {};
    return InetNtopA(AF_INET, &address, canonical, sizeof(canonical)) != nullptr
        ? std::string(canonical)
        : std::string{};
}

std::string FetchPublicWinInet()
{
    HINTERNET net = InternetOpenA(
        "TrialLauncher/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (net == nullptr)
    {
        return {};
    }

    // Bounded so a minimal Wine WinINet backend cannot delay the plain-Winsock fallback.
    DWORD timeoutMs = 3000u;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    std::string out;
    HINTERNET url = InternetOpenUrlA(
        net,
        "http://api.ipify.org",
        nullptr,
        0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE,
        0);
    if (url != nullptr)
    {
        char buf[128] = {};
        DWORD read = 0;
        if (InternetReadFile(url, buf, sizeof(buf) - 1, &read) && read > 0)
        {
            buf[read] = '\0';
            out.assign(buf);
        }
        InternetCloseHandle(url);
    }
    InternetCloseHandle(net);
    return NormalizePublicIpv4(std::move(out));
}

bool WaitForConnect(const SOCKET socket, const DWORD timeoutMs)
{
    fd_set writable{};
    fd_set failed{};
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket, &writable);
    FD_SET(socket, &failed);

    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeoutMs / 1000u);
    timeout.tv_usec = static_cast<long>((timeoutMs % 1000u) * 1000u);
    if (select(0, nullptr, &writable, &failed, &timeout) <= 0 || FD_ISSET(socket, &failed))
    {
        return false;
    }

    int socketError = 0;
    int errorLength = sizeof(socketError);
    return getsockopt(
               socket,
               SOL_SOCKET,
               SO_ERROR,
               reinterpret_cast<char*>(&socketError),
               &errorLength) == 0
        && socketError == 0;
}

std::string FetchPublicWinsock()
{
    const std::wstring resolved = util::ResolveConnectHostIpv4(L"api.ipify.org");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(80u);
    if (resolved.empty()
        || InetPtonW(AF_INET, resolved.c_str(), &address.sin_addr) != 1)
    {
        return {};
    }

    SOCKET connected = INVALID_SOCKET;
    SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (candidate != INVALID_SOCKET)
    {
        u_long nonBlocking = 1u;
        ioctlsocket(candidate, FIONBIO, &nonBlocking);
        const int connectResult = connect(
            candidate,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address));
        const int connectError = connectResult == SOCKET_ERROR ? WSAGetLastError() : 0;
        const bool pending = connectError == WSAEWOULDBLOCK
            || connectError == WSAEINPROGRESS
            || connectError == WSAEINVAL;
        if (connectResult == 0 || (pending && WaitForConnect(candidate, 3000u)))
        {
            nonBlocking = 0u;
            ioctlsocket(candidate, FIONBIO, &nonBlocking);
            const DWORD timeoutMs = 3000u;
            setsockopt(
                candidate,
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs));
            setsockopt(
                candidate,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMs),
                sizeof(timeoutMs));
            connected = candidate;
        }
        else
        {
            closesocket(candidate);
        }
    }

    if (connected == INVALID_SOCKET)
    {
        return {};
    }

    constexpr std::string_view request =
        "GET / HTTP/1.1\r\n"
        "Host: api.ipify.org\r\n"
        "User-Agent: TrialLauncher/1.0\r\n"
        "Connection: close\r\n\r\n";
    std::size_t sentTotal = 0u;
    while (sentTotal < request.size())
    {
        const int sent = send(
            connected,
            request.data() + sentTotal,
            static_cast<int>(request.size() - sentTotal),
            0);
        if (sent <= 0)
        {
            closesocket(connected);
            return {};
        }
        sentTotal += static_cast<std::size_t>(sent);
    }

    std::string response;
    char buffer[1024] = {};
    while (response.size() < 8192u)
    {
        const int received = recv(connected, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    closesocket(connected);

    const std::size_t statusEnd = response.find("\r\n");
    const std::size_t body = response.find("\r\n\r\n");
    if (statusEnd == std::string::npos || body == std::string::npos
        || response.substr(0u, statusEnd).find(" 200 ") == std::string::npos)
    {
        return {};
    }
    return NormalizePublicIpv4(response.substr(body + 4u));
}

std::pair<std::string, PublicLookupResult> FetchPublic()
{
    std::string address = FetchPublicWinInet();
    if (!address.empty())
    {
        return {std::move(address), PublicLookupResult::WinInet};
    }
    address = FetchPublicWinsock();
    if (!address.empty())
    {
        return {std::move(address), PublicLookupResult::Winsock};
    }
    return {std::string{}, PublicLookupResult::Unavailable};
}

void LogOutcomeOnce(DetectionState& state)
{
    if (state.publicPending.load() || state.outcomeLogged.exchange(true))
    {
        return;
    }
    switch (state.outcome.load())
    {
    case PublicLookupResult::WinInet:
        SessionLog::Step(L"network", L"Public IPv4 detected through WinINet");
        break;
    case PublicLookupResult::Winsock:
        SessionLog::Step(
            L"network",
            L"Public IPv4 detected through bounded Winsock HTTP fallback");
        break;
    default:
        SessionLog::Warn(
            L"network",
            L"Public IPv4 lookup unavailable; online UDP remains usable");
        break;
    }
}

}

void StartDetection()
{
    DetectionState& state = State();
    if (state.started.exchange(true))
    {
        return;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.publicIp = "n/a";
        state.outcome.store(PublicLookupResult::Unavailable);
        return;
    }

    state.publicPending.store(true);
    try
    {
        std::thread([&state]() {
            const std::string lan = DetectLan();
            auto [ip, outcome] = FetchPublic();
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.lan = lan;
                state.publicIp = ip.empty() ? std::string("n/a") : std::move(ip);
                state.outcome.store(outcome);
                state.publicPending.store(false);
            }
            WSACleanup();
        }).detach();
    }
    catch (...)
    {
        WSACleanup();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.publicIp = "n/a";
        state.outcome.store(PublicLookupResult::Unavailable);
        state.publicPending.store(false);
    }
}

std::string LanIp()
{
    DetectionState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.lan;
}

std::string PublicIp()
{
    DetectionState& state = State();
    LogOutcomeOnce(state);
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.publicIp;
}

bool PublicIpPending()
{
    return State().publicPending.load();
}

}
