/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

struct FileStamp
{
    std::uint64_t device;
    std::uint64_t inode;
    std::uint64_t modified;
    std::uint32_t nanoseconds;
    std::uint64_t size;

    FileStamp() : device(0), inode(0), modified(0), nanoseconds(0), size(0)
    {}
    bool operator==(const FileStamp& other) const;
};

// Only metadata is remembered. The original SA-MP code still computes file CRCs.
bool file_stamp_native(std::intptr_t handle, FileStamp& out);
bool file_stamp(const std::string& path, FileStamp& out);

struct Asset
{
    std::string filename;
    std::uint32_t size;
    FileStamp stamp;

    Asset() : size(0)
    {}
    Asset(const std::string& name, std::uint32_t bytes) : filename(name), size(bytes)
    {}
};

class Assets
{
public:
    explicit Assets(std::uintptr_t artwork_global);
    void refresh();                         // server thread only
    void on_model_added(const void* entry); // server thread only
    bool lookup(unsigned type, std::uint32_t crc, Asset& out) const;
    bool lookup_url(const std::string& url, Asset& out) const;
    std::size_t count() const;
    static std::string make_path(unsigned type, std::uint32_t crc);
#ifdef SAMPCDN_TEST
    void add_test_file(const std::string& path, const Asset& asset);
#endif

private:
    std::uintptr_t artwork_global_;
    mutable std::mutex mutex_;
    std::map<std::string, Asset> files_;
    void index_entry(std::map<std::string, Asset>& index, const std::string& root, const unsigned char* entry) const;
};
