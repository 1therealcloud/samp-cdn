/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "fixes.h"
#include "patch.h"
#include <cstdint>
#include <ctime>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace
{
#ifdef _WIN32
    const std::uintptr_t kOriginalHash = 0x00496BC0;
    const std::uintptr_t kCalls[2] = {0x0049723D, 0x00497282};
    const unsigned char kExpected[2][5] = {{0xE8, 0x7E, 0xF9, 0xFF, 0xFF}, {0xE8, 0x39, 0xF9, 0xFF, 0xFF}};
#else
    const std::uintptr_t kOriginalHash = 0x080B9400;
    const std::uintptr_t kCalls[2] = {0x080B9983, 0x080B99CF};
    const unsigned char kExpected[2][5] = {{0xE8, 0x78, 0xFA, 0xFF, 0xFF}, {0xE8, 0x2C, 0xFA, 0xFF, 0xFF}};
#endif

    struct Stamp
    {
        std::uint64_t size;
        std::uint64_t device;
        std::uint64_t inode;
        std::time_t modified;
        long nanoseconds;

        bool operator==(const Stamp& other) const
        {
            return size == other.size && device == other.device && inode == other.inode && modified == other.modified && nanoseconds == other.nanoseconds;
        }
    };

    struct CachedHash
    {
        Stamp stamp;
        std::uint32_t crc;
        std::uint32_t size;
    };

    std::map<std::string, CachedHash> cache;
    std::mutex cache_mutex;
    bool installed = false;
    bool read_name(const char* chars, std::string& name)
    {
        if (!chars)
            return false;
        std::size_t length = 0;
        while (length < 260 && chars[length])
            ++length;
        if (length == 260)
            return false;
        name.assign(chars, length);
        return !name.empty();
    }

    bool file_stamp(const std::string& path, Stamp& stamp)
    {
        struct stat info;
        if (stat(path.c_str(), &info) != 0 || info.st_size < 0)
            return false;
        stamp.size = static_cast<std::uint64_t>(info.st_size);
        stamp.device = static_cast<std::uint64_t>(info.st_dev);
        stamp.inode = static_cast<std::uint64_t>(info.st_ino);
        stamp.modified = info.st_mtime;
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA attrs;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs))
            return false;
        ULARGE_INTEGER ticks;
        ticks.LowPart = attrs.ftLastWriteTime.dwLowDateTime;
        ticks.HighPart = attrs.ftLastWriteTime.dwHighDateTime;
        stamp.modified = static_cast<std::time_t>(ticks.QuadPart / UINT64_C(10000000));
        stamp.nanoseconds = static_cast<long>((ticks.QuadPart % UINT64_C(10000000)) * 100);
#else
        stamp.nanoseconds = info.st_mtim.tv_nsec;
#endif
        return true;
    }

    template <class Original>
    std::uint32_t cached_hash(void* artwork, const char* name, std::uint32_t* out_crc, Original original)
    {
        if (!artwork || !name || !out_crc)
            return original(artwork, name, out_crc);

        std::string root, filename;
        if (!read_name(static_cast<const char*>(artwork), root) || !read_name(name, filename))
            return original(artwork, name, out_crc);

        const std::string path = root + "/" + filename;
        Stamp before;
        if (!file_stamp(path, before))
            return original(artwork, name, out_crc);

        std::lock_guard<std::mutex> lock(cache_mutex);
        const std::map<std::string, CachedHash>::const_iterator found = cache.find(path);
        if (found != cache.end() && found->second.stamp == before)
        {
            *out_crc = found->second.crc;
            return found->second.size;
        }

        const std::uint32_t size = original(artwork, name, out_crc);
        if (size)
        {
            Stamp after;
            if (file_stamp(path, after) && after == before)
                cache[path] = CachedHash{after, *out_crc, size};
            else
                cache.erase(path);
        }
        else
            cache.erase(path);
        return size;
    }

#ifdef _WIN32
    typedef std::uint32_t(__thiscall* OriginalHash)(void*, const char*, std::uint32_t*);
    std::uint32_t __fastcall hooked_hash(void* artwork, void*, const char* name, std::uint32_t* out_crc)
    {
        const OriginalHash original = reinterpret_cast<OriginalHash>(kOriginalHash);
        try
        {
            return cached_hash(artwork, name, out_crc, original);
        }
        catch (...)
        {
            return original(artwork, name, out_crc);
        }
    }
#else
    typedef std::uint32_t (*OriginalHash)(void*, const char*, std::uint32_t*);
    std::uint32_t hooked_hash(void* artwork, const char* name, std::uint32_t* out_crc)
    {
        const OriginalHash original = reinterpret_cast<OriginalHash>(kOriginalHash);
        try
        {
            return cached_hash(artwork, name, out_crc, original);
        }
        catch (...)
        {
            return original(artwork, name, out_crc);
        }
    }
#endif

}

namespace fixes
{
    bool install()
    {
        if (installed)
            return true;
        for (unsigned i = 0; i != 2; ++i)
        {
            if (std::memcmp(reinterpret_cast<const void*>(kCalls[i]), kExpected[i], 5) != 0)
                return false;
        }
        unsigned char patch[5];
        patching::make_call(kCalls[0], reinterpret_cast<const void*>(&hooked_hash), patch);
        if (!patching::replace_call(kCalls[0], kExpected[0], patch))
            return false;
        patching::make_call(kCalls[1], reinterpret_cast<const void*>(&hooked_hash), patch);
        if (!patching::replace_call(kCalls[1], kExpected[1], patch))
        {
            unsigned char first[5];
            patching::make_call(kCalls[0], reinterpret_cast<const void*>(&hooked_hash), first);
            patching::replace_call(kCalls[0], first, kExpected[0]);
            return false;
        }
        installed = true;
        return true;
    }

    bool remove()
    {
        if (!installed)
            return true;
        bool success = true;
        for (unsigned i = 0; i != 2; ++i)
        {
            unsigned char patch[5];
            patching::make_call(kCalls[i], reinterpret_cast<const void*>(&hooked_hash), patch);
            if (!patching::replace_call(kCalls[i], patch, kExpected[i]))
                success = false;
        }
        if (success)
        {
            installed = false;
            std::lock_guard<std::mutex> lock(cache_mutex);
            cache.clear();
        }
        return success;
    }

#ifdef SAMPCDN_TEST
    std::uint32_t test_hash(void* artwork, const char* name, std::uint32_t* crc, HashFunction original)
    {
        return cached_hash(artwork, name, crc, original);
    }

    void clear_test_cache()
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache.clear();
    }
#endif
}
