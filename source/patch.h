/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#   include <windows.h>
#else
#   include <sys/mman.h>
#   include <unistd.h>
#endif

namespace patching
{
    inline void make_call(std::uintptr_t site, const void* function, unsigned char out[5])
    {
        out[0] = 0xE8;
        const std::uint32_t displacement = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(function) - (site + 5));
        std::memcpy(out + 1, &displacement, 4);
    }

    inline bool replace_call(std::uintptr_t site, const unsigned char expected[5], const unsigned char replacement[5])
    {
        unsigned char* address = reinterpret_cast<unsigned char*>(site);
        if (std::memcmp(address, expected, 5) != 0)
            return false;
#ifdef _WIN32
        DWORD previous;
        if (!VirtualProtect(address, 5, PAGE_EXECUTE_READWRITE, &previous))
            return false;
        std::memcpy(address, replacement, 5);
        FlushInstructionCache(GetCurrentProcess(), address, 5);
        DWORD ignored;
        VirtualProtect(address, 5, previous, &ignored);
#else
        const long size = sysconf(_SC_PAGESIZE);
        if (size <= 0)
            return false;
        const std::uintptr_t mask = static_cast<std::uintptr_t>(size) - 1;
        const std::uintptr_t page = site & ~mask;
        const std::size_t span = ((site + 5 + mask) & ~mask) - page;
        if (mprotect(reinterpret_cast<void*>(page), span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            return false;
        std::memcpy(address, replacement, 5);
        __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address + 5));
        mprotect(reinterpret_cast<void*>(page), span, PROT_READ | PROT_EXEC);
#endif
        // Once bytes are written, report success so callers can undo the patch.
        return true;
    }
}
