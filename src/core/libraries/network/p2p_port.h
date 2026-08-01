// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/types.h"
#include "core/libraries/network/sockets.h"

namespace Libraries::Net {

// Several vports share one host port, so every datagram carries this 6-byte header and the
// receiver routes by dst_vport. Layout and flags follow RPCS3.
constexpr u32 kP2PHeaderSize = 6;
constexpr u16 kP2PFlagDgram = 1;
constexpr u16 kP2PFlagStream = 2;

// Largest datagram the host stack carries.
constexpr u32 kP2PMaxDatagram = 65535;

constexpr net_socket kInvalidNetSocket =
#ifdef _WIN32
    INVALID_SOCKET;
#else
    -1;
#endif

// vport 0 is reserved for signaling; binding it asks for an ephemeral vport from here up.
constexpr u16 kP2PSignalingVport = 0;
constexpr u16 kP2PFirstEphemeralVport = 30000;

// Prepended on the loopback hop into an inbox so the sender survives it. Never on the wire.
struct P2PInboxHeader {
    u32 from_addr;  // network byte order
    u16 from_port;  // network byte order
    u16 from_vport; // network byte order
};
static_assert(sizeof(P2PInboxHeader) == 8, "P2PInboxHeader must be tightly packed");

/// One host UDP socket per (address, port), shared by every vport bound on it. Incoming
/// datagrams are routed by dst_vport into each endpoint's inbox socket, the fd the guest polls.
class P2PPort {
public:
    /// Returns the shared port for `port` (network byte order, 0 for an ephemeral one), binding
    /// it on first use. Returns nullptr with the host error set on failure.
    static std::shared_ptr<P2PPort> Acquire(u32 addr, u16 port);

    P2PPort(u32 addr, u16 port, net_socket sock);
    ~P2PPort();

    P2PPort(const P2PPort&) = delete;
    P2PPort& operator=(const P2PPort&) = delete;

    /// Claims `vport` (0 for an ephemeral one) for `owner`, forwarding its datagrams to `inbox`.
    /// Returns the vport in network byte order, or 0 when a socket that did not opt into sharing
    /// holds it.
    u16 Claim(u16 vport, bool reusable, const void* owner, const sockaddr_in& inbox);
    void Release(const void* owner);

    /// Sends one datagram with the header prepended. Returns `len` on success, or -1 with the
    /// host error set.
    int Send(const void* data, u32 len, u16 src_vport, u16 dst_vport, const sockaddr_in& dst,
             u16 flags);

    u32 BoundAddr() const {
        return bound_addr;
    }
    u16 BoundPort() const {
        return bound_port;
    }

private:
    void ReceiveLoop();
    void WakeReceiver();

    struct Endpoint {
        const void* owner;
        u16 vport; // network byte order
        bool reusable;
        sockaddr_in inbox;
    };

    u32 bound_addr{}; // network byte order
    u16 bound_port{}; // network byte order
    net_socket sock;
    std::mutex mutex;
    std::vector<Endpoint> endpoints;
    std::atomic<bool> stop{false};
    std::thread receiver;
};

/// True when `sock` is a usable socket handle.
bool IsValidP2PSocket(net_socket sock);
/// Creates the loopback inbox a guest P2P socket exposes as its fd and reports its address.
/// Returns false with the host error set on failure.
bool CreateP2PInbox(net_socket& sock, sockaddr_in& addr);
void CloseP2PSocket(net_socket sock);
/// Waits for `sock` to become readable. `timeout_us` < 0 waits indefinitely, 0 only polls.
bool WaitP2PSocketReadable(net_socket sock, s64 timeout_us);

} // namespace Libraries::Net
