// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "common/types.h"

namespace Common {

// Like std::shared_mutex, but reader has priority over writer.
// Readers only touch the mutex while a writer holds the lock.
class SharedFirstMutex {
public:
    void lock() {
        std::unique_lock<std::mutex> lock(mtx);
        ++writers_waiting;
        cv.wait(lock, [this]() { return TryLockExclusive(); });
        --writers_waiting;
    }

    bool try_lock() {
        return TryLockExclusive();
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        std::unique_lock<std::mutex> lock(mtx);
        ++writers_waiting;
        const bool locked = cv.wait_until(lock, abs_time, [this]() { return TryLockExclusive(); });
        --writers_waiting;
        return locked;
    }

    void unlock() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            state.store(0);
        }
        cv.notify_all();
    }

    void lock_shared() {
        if (TryLockShared()) {
            return;
        }
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return TryLockShared(); });
    }

    bool try_lock_shared() {
        return TryLockShared();
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
        if (TryLockShared()) {
            return true;
        }
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_until(lock, abs_time, [this]() { return TryLockShared(); });
    }

    void unlock_shared() {
        if (state.fetch_sub(1) == 1 && writers_waiting.load() != 0) {
            // Taking the mutex makes sure the writer is already waiting on the cv.
            std::lock_guard<std::mutex> lock(mtx);
            cv.notify_all();
        }
    }

private:
    static constexpr u32 WriterActive = 1u << 31;

    bool TryLockShared() {
        u32 current = state.load(std::memory_order_relaxed);
        while ((current & WriterActive) == 0) {
            if (state.compare_exchange_weak(current, current + 1)) {
                return true;
            }
        }
        return false;
    }

    bool TryLockExclusive() {
        u32 expected = 0;
        return state.compare_exchange_strong(expected, WriterActive);
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<u32> state{};
    std::atomic<u32> writers_waiting{};
};

} // namespace Common
