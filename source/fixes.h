/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once

#include <cstdint>

namespace fixes
{
    bool install();
    bool remove();

#ifdef SAMPCDN_TEST
    typedef std::uint32_t (*HashFunction)(void*, const char*, std::uint32_t*);
    std::uint32_t test_hash(void* artwork, const char* name, std::uint32_t* crc, HashFunction original);
    void clear_test_cache();
#endif
}
