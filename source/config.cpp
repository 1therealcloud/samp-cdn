/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "config.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <windows.h>
#else
#   include <arpa/inet.h>
#   include <dlfcn.h>
#   include <limits.h>
#   include <sys/socket.h>
#   include <unistd.h>
#endif

namespace
{
    std::string trim(const std::string& s)
    {
        const std::string ws = " \t\r\n";
        const std::size_t b = s.find_first_not_of(ws);
        if (b == std::string::npos)
            return std::string();
        return s.substr(b, s.find_last_not_of(ws) - b + 1);
    }

    bool parse_line(const std::string& line, std::string& key, std::string& value)
    {
        const std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned[0] == '#' || cleaned[0] == ';')
            return false;
        const std::size_t eq = cleaned.find('=');
        if (eq != std::string::npos)
        {
            key = trim(cleaned.substr(0, eq));
            value = trim(cleaned.substr(eq + 1));
        }
        else
        {
            const std::size_t space = cleaned.find_first_of(" \t");
            if (space == std::string::npos)
                return false;
            key = trim(cleaned.substr(0, space));
            value = trim(cleaned.substr(space + 1));
        }
        return !key.empty();
    }

    bool parse_port(const std::string& str, unsigned& port)
    {
        if (str.empty() || str[0] == '-')
            return false;
        char* end = 0;
        errno = 0;
        const unsigned long value = std::strtoul(str.c_str(), &end, 10);
        if (errno || !end || *end || value == 0 || value > 65535)
            return false;
        port = static_cast<unsigned>(value);
        return true;
    }

    bool valid_ipv4(const std::string& ip)
    {
        in_addr addr;
        return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
    }

    std::string directory_of(const std::string& path)
    {
        const std::size_t pos = path.find_last_of("/\\");
        return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
    }

    std::string path_join(const std::string& dir, const char* name)
    {
        return dir + "/" + name;
    }

    std::string module_directory()
    {
#ifdef _WIN32
        HMODULE module = 0;
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(&module_directory), &module))
            return std::string();
        char path[MAX_PATH];
        const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return std::string();
        return directory_of(std::string(path, length));
#else
        Dl_info info;
        if (!dladdr(reinterpret_cast<const void*>(&module_directory), &info) || !info.dli_fname)
            return std::string();
        char resolved[PATH_MAX];
        const char* filename = realpath(info.dli_fname, resolved);
        return directory_of(filename ? filename : info.dli_fname);
#endif
    }

    std::string executable_directory()
    {
#ifdef _WIN32
        char path[MAX_PATH];
        const DWORD length = GetModuleFileNameA(0, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return std::string();
        return directory_of(std::string(path, length));
#else
        char path[PATH_MAX];
        const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path) - 1)
            return std::string();
        path[length] = 0;
        return directory_of(path);
#endif
    }

    bool validate_url(std::string& url)
    {
        if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
            return false;
        while (!url.empty() && url[url.size() - 1] == '/')
            url.resize(url.size() - 1);
        if (url.find_first_of(" \t\r\n?#") != std::string::npos || url.size() > 160)
            return false;
        const std::size_t proto_end = url.find("://") + 3;
        return url.size() > proto_end && url.find('/', proto_end) == std::string::npos && url.find('@', proto_end) == std::string::npos;
    }
}

bool load_config_file(const std::string& path, Settings& settings, std::string& error)
{
    settings.config_path = path;
    std::ifstream config(path.c_str());
    if (!config)
    {
        // First launch: create a genuinely empty configuration file, without failing startup.
        std::ofstream create(path.c_str(), std::ios::out | std::ios::app);
        if (!create)
        {
            error = "cannot read or create " + path;
            return false;
        }
        create.close();
        config.open(path.c_str());
        if (!config)
        {
            error = "cannot read " + path;
            return false;
        }
    }
    std::string line;
    while (std::getline(config, line))
    {
        std::string key, value;
        if (!parse_line(line, key, value))
            continue;
        if (key == "public_url")
            settings.public_url = value;
        else if (key == "bind")
        {
            if (!valid_ipv4(value))
            {
                error = "invalid bind in " + path;
                return false;
            }
            settings.bind_ip = value;
        }
        // Do not configure the HTTP port separately; always use the game's UDP port number.
    }
    if (!settings.public_url.empty() && !validate_url(settings.public_url))
    {
        error = "invalid public_url in " + path;
        return false;
    }
    return true;
}

void load_server_config_file(const std::string& path, unsigned& port)
{
    std::ifstream config(path.c_str());
    if (!config)
        return; // Use the SA-MP default when server.cfg is absent.
    std::string line;
    while (std::getline(config, line))
    {
        std::string key, value;
        if (!parse_line(line, key, value))
            continue;
        if (key == "port")
        {
            unsigned n;
            if (parse_port(value, n))
                port = n;
        }
    }
}

std::string make_public_url(const std::string& specified_url, unsigned port)
{
    if (!specified_url.empty())
        return specified_url;
    return "http://localhost:" + std::to_string(port);
}

bool load_settings(Settings& settings, std::string& error)
{
    const std::string module_dir = module_directory();
    if (module_dir.empty())
    {
        error = "cannot determine plugin directory";
        return false;
    }
    if (!load_config_file(path_join(module_dir, "cdn.cfg"), settings, error))
        return false;
    const std::string server_dir = executable_directory();
    // SA-MP normally reads server.cfg relative to its working directory.
    std::ifstream cwd_config("server.cfg");
    if (cwd_config.good())
        load_server_config_file("server.cfg", settings.port);
    else if (!server_dir.empty())
        load_server_config_file(path_join(server_dir, "server.cfg"), settings.port);
    settings.public_url = make_public_url(settings.public_url, settings.port);
    return true;
}
