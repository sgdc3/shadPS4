// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {

template <class T>
class Singleton {
public:
    static T* Instance() {
        // A function-local static: the C++11 guarantees make the first, concurrent call safe.
        // The lazy unique_ptr this replaces let two first callers both construct the instance,
        // and the losing thread kept using the copy the assignment had just destroyed.
        static T instance;
        return &instance;
    }

protected:
    Singleton();
    ~Singleton();
};

} // namespace Common
