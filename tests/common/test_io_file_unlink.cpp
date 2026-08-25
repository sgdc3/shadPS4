// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Unlinking has to end with the file gone. On Windows the request is a disposition set on the
// open handle, and that handle carries no DELETE access, so the request comes back refused: a
// caller that ignores the status leaves the file in place while reporting success. Games that
// save by writing the next generation of a file and unlinking the previous one then keep every
// generation forever and reload a stale one.

#include <filesystem>

#include <gtest/gtest.h>

#include "common/io_file.h"
#include "common/types.h"

#ifdef _WIN32
#include "common/ntapi.h"
#endif

namespace fs = std::filesystem;

using Common::FS::FileAccessMode;
using Common::FS::IOFile;

namespace {

fs::path WriteTempFile(std::string_view name) {
    const auto path = fs::temp_directory_path() / name;
    fs::remove(path);

    IOFile file{path, FileAccessMode::Create};
    const u8 payload[] = {'f', 'a', 'r', 't'};
    file.WriteRaw<u8>(payload, sizeof(payload));
    file.Close();

    return path;
}

class IOFileUnlink : public testing::Test {
protected:
    static void SetUpTestSuite() {
#ifdef _WIN32
        // Unlink goes through the ntdll thunks, which the emulator resolves at startup.
        Common::NtApi::Initialize();
#endif
    }
};

} // namespace

TEST_F(IOFileUnlink, RemovesTheFileOnceTheHandleIsClosed) {
    const auto path = WriteTempFile("shadps4_unlink_closes.bin");
    ASSERT_TRUE(fs::exists(path));

    {
        IOFile file{path, FileAccessMode::ReadWrite};
        ASSERT_TRUE(file.IsOpen());
        file.Unlink();
    }

    EXPECT_FALSE(fs::exists(path));
}

TEST_F(IOFileUnlink, DoesNothingWithoutAnOpenFile) {
    const auto path = WriteTempFile("shadps4_unlink_unopened.bin");

    IOFile file;
    file.Unlink();

    EXPECT_TRUE(fs::exists(path));
    fs::remove(path);
}

TEST_F(IOFileUnlink, ReopeningTakesTheFirstFileWithIt) {
    const auto first = WriteTempFile("shadps4_unlink_reopen_first.bin");
    const auto second = WriteTempFile("shadps4_unlink_reopen_second.bin");

    IOFile file{first, FileAccessMode::ReadWrite};
    file.Unlink();
    file.Open(second, FileAccessMode::ReadWrite);

    EXPECT_FALSE(fs::exists(first));
    EXPECT_TRUE(fs::exists(second));

    file.Close();
    EXPECT_TRUE(fs::exists(second));
    fs::remove(second);
}
