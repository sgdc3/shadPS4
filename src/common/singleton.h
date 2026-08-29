// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {

template <class T>
class Singleton {
public:
    static T* Instance() {
        // A function-local static makes a concurrent first call safe; the lazy unique_ptr let
        // two callers construct it and one keep the destroyed copy.
        static T instance;
        return &instance;
    }

protected:
    Singleton();
    ~Singleton();
};

} // namespace Common
