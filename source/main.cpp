/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "artwork.h"
#include "config.h"
#include "fixes.h"
#include "http.h"
#include "index_hook.h"
#include "plugincommon.h"
#include "patch.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#ifdef _WIN32
#   include <windows.h>
#   define PLUGIN_ENTRY extern "C" __declspec(dllexport)
#else
#   define PLUGIN_ENTRY extern "C" __attribute__((visibility("default")))
#endif

namespace
{
#ifdef _WIN32
    const std::uintptr_t kArtwork = 0x004FC444;
    const std::uintptr_t kNetGame = 0x004FC438;
    const std::uintptr_t kCallSite = 0x00496FF9;
    const std::uintptr_t kCallback = 0x0046D0F0;
    const std::uintptr_t kRedirect = 0x00488240;
    const unsigned char kOriginalCall[5] = {0xE8, 0xF2, 0x60, 0xFD, 0xFF};
    const std::uint64_t kServerHash = UINT64_C(0xD0CA727A2100220F);
#else
    const std::uintptr_t kArtwork = 0x081D7934;
    const std::uintptr_t kNetGame = 0x081D7940;
    const std::uintptr_t kCallSite = 0x080B9244;
    const std::uintptr_t kCallback = 0x080ACA60;
    const std::uintptr_t kRedirect = 0x080D9EF0;
    const unsigned char kOriginalCall[5] = {0xE8, 0x17, 0x38, 0xFF, 0xFF};
    const std::uint64_t kServerHash = UINT64_C(0x8ACC5D8D4C5C8D4F);
#endif
    const std::uint32_t kMaxPlayers = 1000;
    const std::uint32_t kPlayerSlotOffset = 0x25D84;
    const std::uint32_t kDownloadStateOffset = 0x30CF;
    const std::uint32_t kRedirectedOffset = 0x1028;

    typedef void (*LogPrint)(const char*, ...);
    LogPrint plugin_logprintf = nullptr;
#define logprintf plugin_logprintf
    std::unique_ptr<Assets> assets_;
    std::unique_ptr<CHttpServer> http_;
    std::string public_url_;
    bool hook_installed_ = false;

    bool check_server_binary()
    {
#ifdef _WIN32
        char path[MAX_PATH];
        const DWORD len = GetModuleFileNameA(0, path, MAX_PATH);
        if (!len || len == MAX_PATH)
            return false;
        FILE* file = std::fopen(path, "rb");
#else
        FILE* file = std::fopen("/proc/self/exe", "rb");
#endif
        if (!file)
            return false;
        std::uint64_t hash = UINT64_C(0xCBF29CE484222325);
        unsigned char buffer[32768];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            for (std::size_t i = 0; i < n; ++i)
            {
                hash ^= buffer[i];
                hash *= UINT64_C(0x100000001B3);
            }
        }
        const bool good = !std::ferror(file) && hash == kServerHash;
        std::fclose(file);
        return good;
    }

    unsigned char* get_download_state(unsigned playerid)
    {
        if (playerid >= kMaxPlayers)
            return 0;
        unsigned char* netgame = *reinterpret_cast<unsigned char* const*>(kNetGame);
        if (!netgame)
            return 0;
        unsigned char* players = *reinterpret_cast<unsigned char* const*>(netgame + 8);
        if (!players)
            return 0;
        unsigned char* player = *reinterpret_cast<unsigned char* const*>(players + kPlayerSlotOffset + playerid * 4);
        if (!player)
            return 0;
        return *reinterpret_cast<unsigned char* const*>(player + kDownloadStateOffset);
    }

    void redirect(unsigned char* state, const std::string& url)
    {
#ifdef _WIN32
        typedef void(__fastcall * Function)(unsigned char*, void*, const char*);
        reinterpret_cast<Function>(kRedirect)(state, 0, url.c_str());
#else
        typedef void (*Function)(unsigned char*, const char*);
        reinterpret_cast<Function>(kRedirect)(state, url.c_str());
#endif
    }

    void automatic_redirect(unsigned playerid, unsigned type, std::uint32_t crc)
    {
        if (!assets_ || (type != 1 && type != 2))
            return;
        unsigned char* state = get_download_state(playerid);
        if (!state || state[kRedirectedOffset])
            return; // respect an earlier Pawn redirect
        Asset file;
        if (!assets_->lookup(type, crc, file))
            return; // let the original RakNet download handle it
        const std::string url = public_url_ + Assets::make_path(type, crc);
        if (url.size() > 255)
            return; // RedirectDownload serializes URL length in a byte.
        redirect(state, url);
    }

#ifdef _WIN32
    // The original callback is __thiscall: ECX holds the dispatcher, three stack arguments.
    typedef int(__fastcall* OriginalCallback)(void*, void*, int, int, std::uint32_t);
    int __fastcall download_callback(void* dispatcher, void*, int playerid, int type, std::uint32_t crc)
    {
        const int result = reinterpret_cast<OriginalCallback>(kCallback)(dispatcher, 0, playerid, type, crc);
        if (playerid >= 0)
        {
            try
            {
                automatic_redirect(static_cast<unsigned>(playerid), static_cast<unsigned>(type), crc);
            }
            catch (...)
            {
                logprintf("[sampcdn] redirect failed; SA-MP fallback remains available");
            }
        }
        return result;
    }
#else
    typedef int (*OriginalCallback)(void*, int, int, std::uint32_t);
    int download_callback(void* dispatcher, int playerid, int type, std::uint32_t crc)
    {
        const int result = reinterpret_cast<OriginalCallback>(kCallback)(dispatcher, playerid, type, crc);
        if (playerid >= 0)
        {
            try
            {
                automatic_redirect(static_cast<unsigned>(playerid), static_cast<unsigned>(type), crc);
            }
            catch (...)
            {
                logprintf("[sampcdn] redirect failed; SA-MP fallback remains available");
            }
        }
        return result;
    }
#endif

    bool install_hook()
    {
        const unsigned char* at = reinterpret_cast<const unsigned char*>(kCallSite);
        if (std::memcmp(at, kOriginalCall, 5) != 0)
        {
            logprintf("[sampcdn] unexpected callback bytes: wrong server version or conflicting patch");
            return false;
        }
        unsigned char patch[5];
        patching::make_call(kCallSite, reinterpret_cast<const void*>(&download_callback), patch);
        if (!patching::replace_call(kCallSite, kOriginalCall, patch))
            return false;
        hook_installed_ = true;
        return true;
    }

    bool remove_hook()
    {
        if (!hook_installed_)
            return true;
        unsigned char patch[5];
        patching::make_call(kCallSite, reinterpret_cast<const void*>(&download_callback), patch);
        if (!patching::replace_call(kCallSite, patch, kOriginalCall))
            return false;
        hook_installed_ = false;
        return true;
    }
} // namespace

PLUGIN_ENTRY unsigned int PLUGIN_CALL Supports()
{
    return SUPPORTS_VERSION;
}

PLUGIN_ENTRY bool PLUGIN_CALL Load(void** data)
{
    if (!data)
        return false;
    logprintf = reinterpret_cast<LogPrint>(data[PLUGIN_DATA_LOGPRINTF]);
    if (!logprintf)
        return false;
    Settings settings;
    std::string error;
    if (!load_settings(settings, error))
    {
        logprintf("[sampcdn] %s", error.c_str());
        return false;
    }
    if (!check_server_binary())
    {
        logprintf("[sampcdn] unsupported server executable (fingerprint mismatch); refusing to patch");
        return false;
    }
    assets_.reset(new Assets(kArtwork));
    assets_->refresh();
    http_.reset(new CHttpServer(*assets_));
    if (!http_->start(settings.bind_ip, static_cast<std::uint16_t>(settings.port), error))
    {
        logprintf("[sampcdn] %s", error.c_str());
        http_.reset();
        assets_.reset();
        return false;
    }
    public_url_ = settings.public_url;
    if (!install_hook())
    {
        http_->stop();
        http_.reset();
        assets_.reset();
        return false;
    }
    if (!fixes::install())
    {
        logprintf("[sampcdn] CRC cache hook failed; wrong binary or conflicting patch");
        if (!remove_hook())
            logprintf("[sampcdn] ERROR: could not restore redirect hook");
        http_->stop();
        http_.reset();
        assets_.reset();
        return false;
    }
    if (!index_hook::install(*assets_))
    {
        logprintf("[sampcdn] artwork index hook failed; unexpected server instructions");
        fixes::remove();
        if (!remove_hook())
            logprintf("[sampcdn] ERROR: could not restore redirect hook");
        http_->stop();
        http_.reset();
        assets_.reset();
        return false;
    }
    logprintf("[sampcdn] HTTP ready on %s:%u; redirect %s; config %s; files %zu", settings.bind_ip.c_str(), settings.port, public_url_.c_str(), settings.config_path.c_str(),
              assets_->count());
    return true;
}

PLUGIN_ENTRY void PLUGIN_CALL Unload()
{
    if (!index_hook::remove())
        logprintf("[sampcdn] ERROR: could not restore artwork index hook");
    if (!fixes::remove())
        logprintf("[sampcdn] ERROR: could not restore CRC hooks");
    if (!remove_hook())
        logprintf("[sampcdn] ERROR: could not restore redirect hook");
    if (http_)
        http_->stop();
    http_.reset();
    assets_.reset();
    public_url_.clear();
    logprintf("[sampcdn] unloaded");
}
