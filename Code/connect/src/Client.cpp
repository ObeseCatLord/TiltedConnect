#include <chrono>
#include "Client.hpp"
#include "ClientAttemptPolicy.hpp"
#include "SteamInterface.hpp"
#include <steam/isteamnetworkingutils.h>
#include <cassert>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Allocator.hpp>
#include <uv.h>
#include "Packet.hpp"
#include <google/protobuf/stubs/port.h>
#include <snappy.h>

namespace TiltedPhoques
{
    static thread_local Client* s_pClient = nullptr;

    Client::Client() noexcept
    {
        SteamInterface::Acquire();

        m_connection = k_HSteamNetConnection_Invalid;
        m_pInterface = SteamNetworkingSockets();

        m_pLoop = Allocator::GetDefault()->Allocate(sizeof(uv_loop_t));
        auto* pLoop = static_cast<uv_loop_t*>(m_pLoop);
        uv_loop_init(pLoop);
        pLoop->data = this;
    }

    Client::~Client()
    {
        m_shuttingDown = true;
        ++m_resolveGeneration;
        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->CloseConnection(m_connection, 0, nullptr, false);
            m_connection = k_HSteamNetConnection_Invalid;
        }

        auto* pLoop = static_cast<uv_loop_t*>(m_pLoop);
        if (!pLoop)
        {
            SteamInterface::Release();
            return;
        }

        if (m_pHandle != nullptr)
            uv_cancel(static_cast<uv_req_t*>(m_pHandle));

        for (uint32_t attempt = 0; attempt < 250 && uv_loop_alive(pLoop); ++attempt)
        {
            uv_run(pLoop, UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // A still-busy loop may be owned by an uncancellable resolver worker.
        // Leaking it at process teardown is safer than freeing live libuv state.
        if (uv_loop_close(pLoop) == 0)
            Allocator::Get()->Free(m_pLoop);
        m_pLoop = nullptr;
        SteamInterface::Release();
    }

    Client::Client(Client&& aRhs) noexcept
        : m_connection(k_HSteamNetConnection_Invalid)
        , m_pInterface(nullptr)
    {
        SteamInterface::Acquire();

        this->operator=(std::move(aRhs));
    }

    Client& Client::operator=(Client&& aRhs) noexcept
    {
        std::swap(m_connection, aRhs.m_connection);
        std::swap(m_pInterface, aRhs.m_pInterface);
        std::swap(m_ownerThread, aRhs.m_ownerThread);
        std::swap(m_shuttingDown, aRhs.m_shuttingDown);

        return *this;
    }

    void Client::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* apInfo)
    {
        if (!apInfo || apInfo->m_hConn == k_HSteamNetConnection_Invalid) [[unlikely]]
            return;

        if (s_pClient) [[likely]]
        {
            s_pClient->OnSteamNetConnectionStatusChanged(apInfo);
        }
    }

    bool Client::Connect(const std::string& acEndpoint) noexcept
    {
        AssertOwnerThread();
        if (!ClientAttemptPolicy::CanStartConnection(
                m_connection, k_HSteamNetConnection_Invalid))
            return false;
        static auto GetAddrInfoCallback = [](uv_getaddrinfo_t* apHandle, int aStatus, struct addrinfo* apResult)
        {
            SteamNetworkingIPAddr remoteAddress{};
            bool valid = false;

            auto* pClient = static_cast<Client*>(apHandle->loop->data);
            const auto requestGeneration = static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(apHandle->data));
            const bool isCurrentRequest = pClient && ClientAttemptPolicy::IsCurrentResolveRequest(
                pClient->m_shuttingDown,
                pClient->m_pHandle == apHandle,
                requestGeneration,
                pClient->m_resolveGeneration);
            if (isCurrentRequest)
                pClient->m_pHandle = nullptr;
            
            if (aStatus == 0)
            {
                switch (apResult->ai_family)
                {
                case AF_INET:
                {
                    const auto port = ntohs(reinterpret_cast<sockaddr_in*>(apResult->ai_addr)->sin_port);
                    remoteAddress.SetIPv4(ntohl(reinterpret_cast<sockaddr_in*>(apResult->ai_addr)->sin_addr.s_addr), port);
                    valid = true;
                } break;
                case AF_INET6:
                {
                    const auto port = ntohs(reinterpret_cast<sockaddr_in6*>(apResult->ai_addr)->sin6_port);
                    remoteAddress.SetIPv6(reinterpret_cast<sockaddr_in6*>(apResult->ai_addr)->sin6_addr.s6_addr, port);
                    valid = true;
                } break;
                }
            }

            if (apResult)
                uv_freeaddrinfo(apResult);
            Allocator::GetDefault()->Free(apHandle);

            // uv_cancel cannot stop every resolver worker. A completion from a
            // retired request must never clear a replacement request or open
            // a socket for the retired endpoint.
            if (!isCurrentRequest)
                return;

            if(aStatus == UV_ECANCELED)
            {
                pClient->OnDisconnected(kAborted);
                return;
            }

            if (!valid)
            {
                pClient->OnDisconnected(kCannotResolve);
                return;
            }

            const auto cConnectResult = pClient->Connect(remoteAddress);
            if (!cConnectResult)
            {
                pClient->OnDisconnected(kLocalProblem);
            }
        };

        if (m_pHandle != nullptr)
        {
            auto* pPreviousHandle = static_cast<uv_getaddrinfo_t*>(m_pHandle);
            m_pHandle = nullptr;
            uv_cancel(reinterpret_cast<uv_req_t*>(pPreviousHandle));
        }

        auto resolveGeneration = ++m_resolveGeneration;
        if (resolveGeneration == 0)
            resolveGeneration = ++m_resolveGeneration;

        const auto pos = acEndpoint.find_last_of(':');
        auto* pHandle = static_cast<uv_getaddrinfo_t*>(Allocator::GetDefault()->Allocate(sizeof(uv_getaddrinfo_t)));

        std::string endpoint = acEndpoint;
        std::string serviceName = "25681";
        if(pos != std::string::npos)
        {
            serviceName = acEndpoint.c_str() + 1 + pos;
            endpoint = endpoint.substr(0, pos);
        }

        pHandle->data = reinterpret_cast<void*>(static_cast<uintptr_t>(resolveGeneration));
        m_pHandle = pHandle;
        const auto resolveResult = uv_getaddrinfo(
            static_cast<uv_loop_t*>(m_pLoop), pHandle, GetAddrInfoCallback,
            endpoint.c_str(), serviceName.c_str(), nullptr);
        if (resolveResult != 0)
        {
            m_pHandle = nullptr;
            Allocator::GetDefault()->Free(pHandle);
            OnDisconnected(kCannotResolve);
            return false;
        }

        return true;
    }

    bool Client::Connect(const SteamNetworkingIPAddr& acEndpoint) noexcept
    {
        AssertOwnerThread();
        if (!ClientAttemptPolicy::CanStartConnection(
                m_connection, k_HSteamNetConnection_Invalid))
            return false;
        // A direct-IP attempt is also a resolver boundary. Without this, an
        // older hostname lookup can complete later and overwrite the socket
        // even though the caller has already selected another endpoint.
        if (m_pHandle != nullptr)
        {
            auto* pPreviousHandle = static_cast<uv_getaddrinfo_t*>(m_pHandle);
            m_pHandle = nullptr;
            ++m_resolveGeneration;
            uv_cancel(reinterpret_cast<uv_req_t*>(pPreviousHandle));
        }
        SteamNetworkingConfigValue_t opt = {};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&SteamNetConnectionStatusChangedCallback));
        m_connection = m_pInterface->ConnectByIPAddress(acEndpoint, 1, &opt);

        return m_connection != k_HSteamNetConnection_Invalid;
    }

    bool Client::ConnectByIp(const std::string& acEndpoint) noexcept
    {
        AssertOwnerThread();
        SteamNetworkingIPAddr remoteAddress{};
        remoteAddress.ParseString(acEndpoint.c_str());

        return Connect(remoteAddress);
    }

    void Client::Close() noexcept
    {
        AssertOwnerThread();
        bool notified = false;
        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->CloseConnection(m_connection, 0, nullptr, true);
            m_connection = k_HSteamNetConnection_Invalid;

            m_clock.Reset();

            OnDisconnected(kAborted);
            notified = true;
        }

        if(m_pHandle != nullptr)
        {
            auto* pHandle = static_cast<uv_req_t*>(m_pHandle);
            m_pHandle = nullptr;
            ++m_resolveGeneration;
            uv_cancel(pHandle);
            if (!notified)
                OnDisconnected(kAborted);
        }
    }

    void Client::Update() noexcept
    {
        AssertOwnerThread();
        m_clock.Update();

        if (m_clock.GetCurrentTick() - m_lastStatisticsPoint >= 1000)
        {
            m_lastStatisticsPoint = m_clock.GetCurrentTick();
            m_previousFrame = m_currentFrame;
            m_currentFrame = {};
        }

        s_pClient = this;
        m_pInterface->RunCallbacks();
        uv_run(static_cast<uv_loop_t*>(m_pLoop), UV_RUN_NOWAIT);
        s_pClient = nullptr;

        while (true)
        {
            ISteamNetworkingMessage* pIncomingMsg = nullptr;
            const auto cMessageCount = m_pInterface->ReceiveMessagesOnConnection(m_connection, &pIncomingMsg, 1);
            if (cMessageCount <= 0 || pIncomingMsg == nullptr)
            {
                // TODO: Handle error when messageCount < 0
                break;
            }

            m_currentFrame.RecvBytes += pIncomingMsg->GetSize();
            m_currentFrame.UncompressedRecvBytes += pIncomingMsg->GetSize();

            HandleMessage(pIncomingMsg->GetData(), pIncomingMsg->GetSize());

            pIncomingMsg->Release();
        }

        OnUpdate();
    }

    void Client::Send(Packet* apPacket, const EPacketFlags acPacketFlags) const noexcept
    {
        AssertOwnerThread();
        m_currentFrame.UncompressedSentBytes += apPacket->m_size;

        if (apPacket->m_pData[0] == kPayload)
        {
            std::string data;
            snappy::Compress(apPacket->GetData(), apPacket->GetSize(), &data);

            if (data.size() < apPacket->GetSize())
            {
                apPacket->m_pData[0] = kCompressedPayload;
                std::copy(std::begin(data), std::end(data), apPacket->GetData());
                apPacket->m_size = (data.size() + 1) & 0xFFFFFFFF;
            }
        }

        m_currentFrame.SentBytes += apPacket->m_size;

        m_pInterface->SendMessageToConnection(m_connection, apPacket->m_pData, apPacket->m_size,
            acPacketFlags == kReliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable, nullptr);
    }

    bool Client::IsConnected() const noexcept
    {
        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            SteamNetConnectionInfo_t info{};
            if (m_pInterface->GetConnectionInfo(m_connection, &info))
            {
                if(info.m_eState == k_ESteamNetworkingConnectionState_Connected)
                {
                    return GetClock().IsSynchronized();
                }
            }
        }

        return false;
    }

    SteamNetConnectionRealTimeStatus_t Client::GetConnectionStatus() const noexcept
    {
        SteamNetConnectionRealTimeStatus_t status{};

        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->GetConnectionRealTimeStatus(m_connection, &status, 0, nullptr);
        }

        return status;
    }

    Client::Statistics Client::GetStatistics() const noexcept
    {
        return m_previousFrame;
    }

    const SynchronizedClock& Client::GetClock() const noexcept
    {
        return m_clock;
    }

    void Client::AssertOwnerThread() const noexcept
    {
        const auto currentThread = std::this_thread::get_id();
        if (m_ownerThread == std::thread::id{})
            m_ownerThread = currentThread;

        assert(m_ownerThread == currentThread && "TiltedConnect::Client used from multiple owner threads");
    }

    void Client::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo)
    {
        // Steam may deliver a queued terminal callback after a replacement
        // connection has already become current. Never let the retired handle
        // close or reset the replacement attempt.
        if (!apInfo || !ClientAttemptPolicy::IsCurrentConnectionCallback(
                apInfo->m_hConn, m_connection, k_HSteamNetConnection_Invalid))
            return;

        switch (apInfo->m_info.m_eState)
        {
        case k_ESteamNetworkingConnectionState_None:
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            m_pInterface->CloseConnection(m_connection, 0, nullptr, false);
            m_connection = k_HSteamNetConnection_Invalid;

            m_clock.Reset();

            if (apInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting)
            {
                OnDisconnected(kTimeout);
            }
            else if (apInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
            {
                OnDisconnected(kLocalProblem);
            }
            else
            {
                OnDisconnected(kKicked);
            }

            break;
        }
        case k_ESteamNetworkingConnectionState_Connecting:
            break;
        case k_ESteamNetworkingConnectionState_Connected:
            // We don't notify here, wait for clock sync
            break;
        default:
            break;
        }
    }

    void Client::HandleMessage(const void* apData, uint32_t aSize) noexcept
    {
        // We handle the cases where packets target the current stack or the user stack
        if (aSize == 0)
            return;

        auto pData = static_cast<const uint8_t*>(apData);

        const auto cOpcode = pData[0];

        pData += 1;
        aSize -= 1;

        switch (cOpcode)
        {
        case kPayload:
            OnConsume(pData, aSize);
            break;
        case kServerTime:
            HandleServerTime(pData, aSize);
            break;
        case kCompressedPayload:
            HandleCompressedPayload(pData, aSize);
            break;
        default:
            assert(false);
            break;
        }
    }

    void Client::HandleServerTime(const void* apData, uint32_t aSize) noexcept
    {
        if (aSize < 8)
            return;

        const auto cConnectionStatus = GetConnectionStatus();

        const auto cServerTime = google::protobuf::BigEndian::Load64(apData);
        const auto cWasSynchronized = GetClock().IsSynchronized();

        m_clock.Synchronize(cServerTime, cConnectionStatus.m_nPing);

        if (!cWasSynchronized)
            OnConnected();
    }

    void Client::HandleCompressedPayload(const void* apData, uint32_t aSize) noexcept
    {
        std::string data;
        snappy::Uncompress((const char*)apData, aSize, &data);

        m_currentFrame.UncompressedRecvBytes -= aSize;
        m_currentFrame.UncompressedRecvBytes += data.size() & 0xFFFFFFFF;

        if (!data.empty()) [[likely]]
        {
            OnConsume(data.data(), data.size() & 0xFFFFFFFF);
        }
    }
}
