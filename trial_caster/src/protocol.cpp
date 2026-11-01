#include "protocol.h"

#include <algorithm>

namespace trial_caster::proto
{

void CopyWideToField(const std::wstring& src, std::array<std::uint16_t, 32>& dest)
{
    dest.fill(0);
    const std::size_t count = std::min<std::size_t>(src.size(), dest.size() - 1);
    for (std::size_t i = 0; i < count; ++i)
    {
        dest[i] = static_cast<std::uint16_t>(src[i]);
    }
}

std::wstring FieldToWide(const std::array<std::uint16_t, 32>& src)
{
    std::wstring out;
    for (std::uint16_t ch : src)
    {
        if (ch == 0)
        {
            break;
        }
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

bool PeerRequiresRollback(const std::array<std::uint16_t, 32>& networkTypeField)
{
    return ParseNetworkMode(networkTypeField) == NetSyncMode::Rollback;
}

NetSyncMode ParseNetworkMode(const std::array<std::uint16_t, 32>& networkTypeField)
{
    const std::wstring peerType = FieldToWide(networkTypeField);
    if (peerType == L"rollback")
    {
        return NetSyncMode::Rollback;
    }
    return NetSyncMode::Lockstep;
}

NetSyncMode LocalNetworkMode(const bool useRollbackNetplay)
{
    return useRollbackNetplay ? NetSyncMode::Rollback : NetSyncMode::Lockstep;
}

std::wstring NetworkModeWireString(const bool useRollbackNetplay)
{
    return useRollbackNetplay ? L"rollback" : L"lockstep";
}

const wchar_t* NetworkModeLabel(const NetSyncMode mode)
{
    switch (mode)
    {
    case NetSyncMode::Rollback:
        return L"rollback";
    case NetSyncMode::Lockstep:
    default:
        return L"lockstep";
    }
}

bool NetworkModesMatch(const NetSyncMode local, const NetSyncMode peer)
{
    return local == peer;
}

}
