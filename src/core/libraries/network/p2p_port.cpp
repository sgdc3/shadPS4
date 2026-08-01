// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>

#ifndef _WIN32
#include <poll.h>
#endif

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/network/p2p_port.h"

namespace Libraries::Net {

// Upper bound on how long the receive thread sleeps without traffic; a stop wakes it.
constexpr s64 kReceivePollTimeoutUs = 100'000;

bool IsValidP2PSocket(net_socket sock) {
    return sock != kInvalidNetSocket;
}

// Read the error before any cleanup: closing the socket or logging can overwrite it.
static int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

constexpr int kSocketErrorMsgSize =
#ifdef _WIN32
    WSAEMSGSIZE;
#else
    EMSGSIZE;
#endif

static void RestoreSocketError(int error) {
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno = error;
#endif
}

void CloseP2PSocket(net_socket sock) {
    if (!IsValidP2PSocket(sock)) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

static bool SetNonBlocking(net_socket sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int mode = 1;
    return ioctl(sock, FIONBIO, &mode) == 0;
#endif
}

bool WaitP2PSocketReadable(net_socket sock, s64 timeout_us) {
    if (!IsValidP2PSocket(sock)) {
        return false;
    }
    // poll rather than select: on POSIX FD_SET writes past the fd_set for descriptors
    // >= FD_SETSIZE, and this runs on every receive.
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;
    const int timeout_ms =
        timeout_us < 0 ? -1 : static_cast<int>(std::min<s64>(timeout_us / 1000, INT_MAX));
#ifdef _WIN32
    const int res = WSAPoll(&pfd, 1, timeout_ms);
#else
    const int res = ::poll(&pfd, 1, timeout_ms);
#endif
    return res > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

bool CreateP2PInbox(net_socket& sock, sockaddr_in& addr) {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!IsValidP2PSocket(sock)) {
        return false;
    }
    // Only the port's receive thread in this process sends here: loopback, OS-chosen port.
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    if (::bind(sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        const int error = LastSocketError();
        CloseP2PSocket(sock);
        sock = kInvalidNetSocket;
        RestoreSocketError(error);
        return false;
    }
    socklen_t len = sizeof(addr);
    std::memset(&addr, 0, sizeof(addr));
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        const int error = LastSocketError();
        CloseP2PSocket(sock);
        sock = kInvalidNetSocket;
        RestoreSocketError(error);
        return false;
    }
    // Blocking is implemented by waiting for readability first, never in the OS call.
    SetNonBlocking(sock);
    return true;
}

// One OS socket per host port, shared by every vport bound on it; a second bind would
// collide with the first.
static std::mutex g_ports_mutex;
static std::map<u64, std::weak_ptr<P2PPort>> g_ports;

static u64 PortKey(u32 addr, u16 port) {
    return (static_cast<u64>(addr) << 16) | port;
}

std::shared_ptr<P2PPort> P2PPort::Acquire(u32 addr, u16 port) {
    // Offline the socket still exists (games probe their own signaling port) but binds the
    // loopback only, so nothing leaves the host and Windows raises no firewall prompt.
    const bool online = EmulatorSettings.IsConnectedToNetwork();
    const u32 bind_addr = online ? addr : htonl(INADDR_LOOPBACK);

    std::scoped_lock lock{g_ports_mutex};
    if (port != 0) {
        if (const auto it = g_ports.find(PortKey(bind_addr, port)); it != g_ports.end()) {
            if (auto existing = it->second.lock()) {
                return existing;
            }
        }
    }

    net_socket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!IsValidP2PSocket(sock)) {
        LOG_ERROR(Lib_Net, "P2P: cannot create the host socket for port {}", ntohs(port));
        return nullptr;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = bind_addr;
    local.sin_port = port;
    // No SO_REUSEADDR: a second instance must fail here rather than steal the first one's
    // datagrams.
    if (::bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        const int error = LastSocketError();
        LOG_ERROR(Lib_Net, "P2P: cannot bind host port {} (already in use by another process?)",
                  ntohs(port));
        CloseP2PSocket(sock);
        RestoreSocketError(error);
        return nullptr;
    }
    socklen_t len = sizeof(local);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
        const int error = LastSocketError();
        CloseP2PSocket(sock);
        RestoreSocketError(error);
        return nullptr;
    }
    SetNonBlocking(sock);

    auto p2p_port = std::make_shared<P2PPort>(local.sin_addr.s_addr, local.sin_port, sock);
    g_ports[PortKey(bind_addr, local.sin_port)] = p2p_port;
    LOG_INFO(Lib_Net, "P2P: host socket bound, addr = {:#010x}, port = {}, online = {}",
             ntohl(local.sin_addr.s_addr), ntohs(local.sin_port), online);
    return p2p_port;
}

P2PPort::P2PPort(u32 addr, u16 port, net_socket sock)
    : bound_addr(addr), bound_port(port), sock(sock) {
    receiver = std::thread([this] { ReceiveLoop(); });
}

P2PPort::~P2PPort() {
    stop.store(true);
    WakeReceiver();
    if (receiver.joinable()) {
        receiver.join();
    }
    CloseP2PSocket(sock);
    LOG_INFO(Lib_Net, "P2P: host socket closed, port = {}", ntohs(bound_port));
}

void P2PPort::WakeReceiver() {
    // An empty datagram is shorter than the header, so the loop drops it after waking up.
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = bound_addr == htonl(INADDR_ANY) ? htonl(INADDR_LOOPBACK) : bound_addr;
    self.sin_port = bound_port;
    sendto(sock, "", 0, 0, reinterpret_cast<const sockaddr*>(&self), sizeof(self));
}

u16 P2PPort::Claim(u16 vport, bool reusable, const void* owner, const sockaddr_in& inbox) {
    std::scoped_lock lock{mutex};
    if (vport == kP2PSignalingVport) {
        // Binding vport 0 asks for an ephemeral one; vport 0 itself stays reserved for signaling.
        u16 candidate = kP2PFirstEphemeralVport;
        while (candidate != 0) {
            const u16 be = htons(candidate);
            if (std::none_of(endpoints.begin(), endpoints.end(),
                             [be](const Endpoint& e) { return e.vport == be; })) {
                vport = be;
                break;
            }
            ++candidate;
        }
        if (vport == kP2PSignalingVport) {
            return 0;
        }
    } else {
        // Several sockets may share a vport only if all of them opted in.
        const bool taken = std::any_of(endpoints.begin(), endpoints.end(), [&](const Endpoint& e) {
            return e.vport == vport && !(e.reusable && reusable);
        });
        if (taken) {
            return 0;
        }
    }
    endpoints.push_back({owner, vport, reusable, inbox});
    return vport;
}

void P2PPort::Release(const void* owner) {
    std::scoped_lock lock{mutex};
    std::erase_if(endpoints, [owner](const Endpoint& e) { return e.owner == owner; });
}

int P2PPort::Send(const void* data, u32 len, u16 src_vport, u16 dst_vport, const sockaddr_in& dst,
                  u16 flags) {
    if (len > kP2PMaxDatagram - kP2PHeaderSize) {
        // Set the host error like every other failure path, or the caller translates a stale one.
        RestoreSocketError(kSocketErrorMsgSize);
        return -1;
    }
    // One buffer per sending thread; scatter-gather sends differ per platform.
    thread_local std::vector<u8> buffer;
    buffer.resize(kP2PHeaderSize + len);
    const u16 header[3] = {dst_vport, src_vport, htons(flags)};
    std::memcpy(buffer.data(), header, kP2PHeaderSize);
    if (len != 0) {
        std::memcpy(buffer.data() + kP2PHeaderSize, data, len);
    }
    const int sent =
        sendto(sock, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()),
               0, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (sent < 0) {
        return -1;
    }
    // The guest counts payload bytes, not what encapsulation added.
    return static_cast<int>(len);
}

void P2PPort::ReceiveLoop() {
    Common::SetCurrentThreadName("shadPS4:P2PPort");
    std::vector<u8> datagram(kP2PMaxDatagram);
    std::vector<u8> forward(sizeof(P2PInboxHeader) + kP2PMaxDatagram);
    std::vector<Endpoint> targets;

    while (!stop.load()) {
        if (!WaitP2PSocketReadable(sock, kReceivePollTimeoutUs)) {
            continue;
        }
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const int received = recvfrom(sock, reinterpret_cast<char*>(datagram.data()),
                                      static_cast<int>(datagram.size()), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received < static_cast<int>(kP2PHeaderSize)) {
            // Our own wake-up, or a stray datagram from something that is not a P2P peer.
            continue;
        }
        u16 header[3];
        std::memcpy(header, datagram.data(), kP2PHeaderSize);
        const u16 dst_vport = header[0];
        const u16 src_vport = header[1];
        const u32 payload_len = static_cast<u32>(received) - kP2PHeaderSize;

        targets.clear();
        {
            std::scoped_lock lock{mutex};
            for (const auto& endpoint : endpoints) {
                if (endpoint.vport == dst_vport) {
                    targets.push_back(endpoint);
                }
            }
        }
        if (targets.empty()) {
            LOG_DEBUG(Lib_Net, "P2P: datagram for unbound vport {} dropped, len = {}",
                      ntohs(dst_vport), payload_len);
            continue;
        }

        const P2PInboxHeader inbox_header{from.sin_addr.s_addr, from.sin_port, src_vport};
        std::memcpy(forward.data(), &inbox_header, sizeof(inbox_header));
        if (payload_len != 0) {
            std::memcpy(forward.data() + sizeof(inbox_header), datagram.data() + kP2PHeaderSize,
                        payload_len);
        }
        const int forward_len = static_cast<int>(sizeof(inbox_header) + payload_len);
        for (const auto& target : targets) {
            // Forward into the endpoint's inbox, which makes the guest fd readable; a full inbox
            // drops the datagram like a real stack would.
            sendto(sock, reinterpret_cast<const char*>(forward.data()), forward_len, 0,
                   reinterpret_cast<const sockaddr*>(&target.inbox), sizeof(target.inbox));
        }
        // Trace: the guest-side receive already logs each datagram at debug.
        LOG_TRACE(Lib_Net, "P2P: datagram routed, vport = {}, len = {}, endpoints = {}",
                  ntohs(dst_vport), payload_len, targets.size());
    }
}

} // namespace Libraries::Net
