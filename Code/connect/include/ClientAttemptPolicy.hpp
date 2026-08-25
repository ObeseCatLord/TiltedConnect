#pragma once

#include <cstdint>

namespace TiltedPhoques::ClientAttemptPolicy
{
[[nodiscard]] constexpr bool IsCurrentResolveRequest(
    const bool aShuttingDown,
    const bool aHandleMatches,
    const std::uint64_t aRequestGeneration,
    const std::uint64_t aCurrentGeneration) noexcept
{
    return !aShuttingDown && aHandleMatches &&
        aRequestGeneration == aCurrentGeneration;
}

template <class T>
[[nodiscard]] constexpr bool IsCurrentConnectionCallback(
    const T aCallbackConnection,
    const T aCurrentConnection,
    const T aInvalidConnection) noexcept
{
    return aCallbackConnection != aInvalidConnection &&
        aCallbackConnection == aCurrentConnection;
}

template <class T>
[[nodiscard]] constexpr bool CanStartConnection(
    const T aCurrentConnection,
    const T aInvalidConnection) noexcept
{
    return aCurrentConnection == aInvalidConnection;
}
}
