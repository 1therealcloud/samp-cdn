/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "artwork.h"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#   include <windows.h>
#else
#   include <fcntl.h>
#   include <sys/stat.h>
#   include <unistd.h>
#endif

namespace
{
    std::uint32_t read_u32(const unsigned char* ptr)
    {
        std::uint32_t value;
        std::memcpy(&value, ptr, sizeof(value));
        return value;
    }

    std::string bounded_string(const unsigned char* ptr, std::size_t capacity)
    {
        std::size_t length = 0;
        while (length < capacity && ptr[length] != 0)
            ++length;
        return length < capacity ? std::string(reinterpret_cast<const char*>(ptr), length) : std::string();
    }

    bool safe_relative(const std::string& name)
    {
        if (name.empty() || name.size() >= 260 || name[0] == '/' || name[0] == '\\')
            return false;
        if (name.find(':') != std::string::npos)
            return false;
        std::string segment;
        for (std::size_t i = 0; i <= name.size(); ++i)
        {
            char c = i == name.size() ? '/' : name[i];
            if (c == '/' || c == '\\')
            {
                if (segment.empty() || segment == "." || segment == "..")
                    return false;
                segment.clear();
            }
            else
            {
                if ((unsigned char)c < 32 || c == '?')
                    return false;
                segment.push_back(c);
            }
        }
        return true;
    }

}

bool FileStamp::operator==(const FileStamp& other) const
{
    return device == other.device && inode == other.inode && modified == other.modified && nanoseconds == other.nanoseconds && size == other.size;
}

bool file_stamp_native(std::intptr_t handle, FileStamp& out)
{
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    out.device = info.dwVolumeSerialNumber;
    out.inode = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    out.modified = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    out.nanoseconds = 0;
    out.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
    struct stat info;
    if (fstat(static_cast<int>(handle), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return false;
    out.device = static_cast<std::uint64_t>(info.st_dev);
    out.inode = static_cast<std::uint64_t>(info.st_ino);
    out.modified = static_cast<std::uint64_t>(info.st_mtime);
    out.nanoseconds = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
    out.size = static_cast<std::uint64_t>(info.st_size);
#endif
    return true;
}

bool file_stamp(const std::string& path, FileStamp& out)
{
#ifdef _WIN32
    HANDLE fd = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (fd == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = file_stamp_native(reinterpret_cast<std::intptr_t>(fd), out);
    CloseHandle(fd);
#else
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = file_stamp_native(fd, out);
    close(fd);
#endif
    return ok;
}

Assets::Assets(std::uintptr_t artwork_global) : artwork_global_(artwork_global)
{}

std::string Assets::make_path(unsigned type, std::uint32_t crc)
{
    char name[48];
    if (type == 1)
        std::snprintf(name, sizeof(name), "/model/%08X.dff", crc);
    else if (type == 2)
        std::snprintf(name, sizeof(name), "/texture/%08X.txd", crc);
    else
        return std::string();
    return name;
}

void Assets::index_entry(std::map<std::string, Asset>& index, const std::string& root, const unsigned char* entry) const
{
    if (!entry)
        return;
    // The type=4 DFF CRC is a filename hash, not a file hash.
    for (unsigned type = 1; type <= 2; ++type)
    {
        if (type == 1 && entry[0] == 4)
            continue;
        const unsigned offset = type == 1 ? 0x0D : 0x112;
        const unsigned crc_offset = type == 1 ? 0x217 : 0x21B;
        const unsigned size_offset = type == 1 ? 0x21F : 0x223;
        const std::string key = make_path(type, read_u32(entry + crc_offset));
        const std::string name = bounded_string(entry + offset, 260);
        if (!safe_relative(name))
            continue;
        const std::uint32_t size = read_u32(entry + size_offset);
        if (!size)
            continue;
        const std::string full_path = root + "/" + name;
        FileStamp stamp;
        if (!file_stamp(full_path, stamp) || stamp.size != size)
            continue;
        // Invalidate an older CRC for the same file after a new AddModel.
        for (std::map<std::string, Asset>::iterator it = index.begin(); it != index.end();)
        {
            if (it->second.filename == full_path && it->first != key)
                it = index.erase(it);
            else
                ++it;
        }
        // Preserve SA-MP's first match for duplicate CRCs.
        if (index.find(key) == index.end())
        {
            Asset asset(full_path, size);
            asset.stamp = stamp;
            index.insert(std::make_pair(key, asset));
        }
    }
}

void Assets::refresh()
{
    const unsigned char* art = *reinterpret_cast<const unsigned char* const*>(artwork_global_);
    if (!art)
        return;
    const std::string root = bounded_string(art, 260);
    if (root.empty())
        return;
    const unsigned char* const* slots = 0;
    std::memcpy(&slots, art + 0x105, sizeof(slots));
    std::uint32_t count = read_u32(art + 0x109);
    if (count > 100000 || (count && !slots))
        return;
    std::map<std::string, Asset> next;
    for (std::uint32_t i = 0; i < count; ++i)
        index_entry(next, root, slots[i]);
    std::lock_guard<std::mutex> lock(mutex_);
    files_.swap(next);
}

void Assets::on_model_added(const void* entry)
{
    const unsigned char* art = *reinterpret_cast<const unsigned char* const*>(artwork_global_);
    if (!art || !entry)
        return;
    const std::string root = bounded_string(art, 260);
    if (root.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    index_entry(files_, root, static_cast<const unsigned char*>(entry));
}

bool Assets::lookup(unsigned type, std::uint32_t crc, Asset& out) const
{
    return lookup_url(make_path(type, crc), out);
}

bool Assets::lookup_url(const std::string& url, Asset& out) const
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::map<std::string, Asset>::const_iterator it = files_.find(url);
        if (it == files_.end())
            return false;
        out = it->second;
    }
    // Do not redirect to a stale CRC after a file is replaced, even if its size matches.
    FileStamp now;
    return file_stamp(out.filename, now) && now == out.stamp;
}

std::size_t Assets::count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return files_.size();
}

#ifdef SAMPCDN_TEST
void Assets::add_test_file(const std::string& path, const Asset& asset)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Asset item = asset;
    if (!file_stamp(asset.filename, item.stamp))
        return;
    files_[path] = item;
}
#endif
