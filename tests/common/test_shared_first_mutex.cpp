// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <thread>

#include <gtest/gtest.h>

#include "common/shared_first_mutex.h"

using namespace std::chrono_literals;

namespace {

TEST(SharedFirstMutex, WriterWaitsForReaders) {
    Common::SharedFirstMutex mutex;
    std::atomic_bool written = false;

    mutex.lock_shared();
    std::thread writer([&] {
        std::scoped_lock lock{mutex};
        written = true;
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(written);
    EXPECT_FALSE(mutex.try_lock());

    // A reader may still enter while the writer is queued, so nested reads cannot deadlock.
    EXPECT_TRUE(mutex.try_lock_shared());
    mutex.unlock_shared();
    mutex.unlock_shared();

    writer.join();
    EXPECT_TRUE(written);
}

TEST(SharedFirstMutex, ReadersWaitForWriter) {
    Common::SharedFirstMutex mutex;
    std::atomic_bool read = false;

    mutex.lock();
    EXPECT_FALSE(mutex.try_lock_shared());
    EXPECT_FALSE(mutex.try_lock_shared_until(std::chrono::steady_clock::now() + 20ms));
    std::thread reader([&] {
        std::shared_lock lock{mutex};
        read = true;
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(read);
    mutex.unlock();

    reader.join();
    EXPECT_TRUE(read);
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST(SharedFirstMutex, TimedWriterGivesUp) {
    Common::SharedFirstMutex mutex;
    mutex.lock_shared();
    EXPECT_FALSE(mutex.try_lock_until(std::chrono::steady_clock::now() + 20ms));
    mutex.unlock_shared();
    EXPECT_TRUE(mutex.try_lock_until(std::chrono::steady_clock::now() + 20ms));
    mutex.unlock();
}

TEST(SharedFirstMutex, ContendedCounterStaysConsistent) {
    Common::SharedFirstMutex mutex;
    int counter = 0;
    std::atomic_int reads = 0;
    constexpr int NumWriters = 4;
    constexpr int NumReaders = 4;
    constexpr int WritesPerThread = 2000;

    std::vector<std::thread> threads;
    for (int i = 0; i < NumWriters; ++i) {
        threads.emplace_back([&] {
            for (int n = 0; n < WritesPerThread; ++n) {
                std::scoped_lock lock{mutex};
                ++counter;
            }
        });
    }
    for (int i = 0; i < NumReaders; ++i) {
        threads.emplace_back([&] {
            for (int n = 0; n < WritesPerThread; ++n) {
                std::shared_lock lock{mutex};
                reads += counter >= 0;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(counter, NumWriters * WritesPerThread);
    EXPECT_EQ(reads, NumReaders * WritesPerThread);
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

} // namespace
