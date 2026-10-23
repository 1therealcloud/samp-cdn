/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "index_hook.h"
#include "artwork.h"
#include "patch.h"
#include <cstdint>
#include <cstring>

namespace
{
#ifdef _WIN32
    const std::uintptr_t call_site = 0x004972C5;
    const std::uintptr_t original_insert = 0x00482100;
    const unsigned char expected[5] = {0xE8, 0x36, 0xAE, 0xFE, 0xFF};
#else
    const std::uintptr_t call_site = 0x080B9A43;
    const std::uintptr_t original_insert = 0x080BA2D0;
    const unsigned char expected[5] = {0xE8, 0x88, 0x08, 0x00, 0x00};
#endif
    Assets* assets_index = 0;
    bool installed = false;

#ifdef _WIN32
    typedef int(__thiscall* Insert)(void*, void*);
    int __fastcall hooked_insert(void* slots, void*, void* entry)
    {
        const int result = reinterpret_cast<Insert>(original_insert)(slots, entry);
        if (result >= 0 && assets_index)
        {
            try
            {
                assets_index->on_model_added(entry);
            }
            catch (...)
            { /* do not change original AddModel behavior */
            }
        }
        return result;
    }
#else
    typedef int (*Insert)(void*, void*);
    int hooked_insert(void* slots, void* entry)
    {
        const int result = reinterpret_cast<Insert>(original_insert)(slots, entry);
        if (result >= 0 && assets_index)
        {
            try
            {
                assets_index->on_model_added(entry);
            }
            catch (...)
            { /* do not change original AddModel behavior */
            }
        }
        return result;
    }
#endif

}

namespace index_hook
{
    bool install(Assets& assets)
    {
        if (installed)
            return true;
        if (std::memcmp(reinterpret_cast<const void*>(call_site), expected, 5) != 0)
            return false;
        unsigned char replacement[5];
        patching::make_call(call_site, reinterpret_cast<const void*>(&hooked_insert), replacement);
        assets_index = &assets;
        if (!patching::replace_call(call_site, expected, replacement))
        {
            assets_index = 0;
            return false;
        }
        installed = true;
        return true;
    }

    bool remove()
    {
        if (!installed)
            return true;
        unsigned char replacement[5];
        patching::make_call(call_site, reinterpret_cast<const void*>(&hooked_insert), replacement);
        if (!patching::replace_call(call_site, replacement, expected))
            return false;
        installed = false;
        assets_index = 0;
        return true;
    }
}
