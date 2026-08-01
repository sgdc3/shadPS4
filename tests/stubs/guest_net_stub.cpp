// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The event queue polls sockets for read and write events; the tests never add one.

#include "core/net/guest_net.h"

namespace Core::Net {

NetResult SocketBytesReadable(s32) {
    return NetResult::Fail(Host::Error::BadF);
}

NetResult SocketSelect(std::span<SelectEntry>, s64) {
    return NetResult::Fail(Host::Error::BadF);
}

} // namespace Core::Net
