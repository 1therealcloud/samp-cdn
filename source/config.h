/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once
#include <string>

struct Settings
{
    std::string public_url;
    std::string bind_ip;
    std::string config_path;
    unsigned port;

    Settings() : bind_ip("0.0.0.0"), port(7777)
    {}
};

// cdn.cfg lives next to the loaded plugin; server.cfg lives next to the server executable.
bool load_settings(Settings& settings, std::string& error);

// Exposed separately so configuration handling can be tested without the SA-MP process.
bool load_config_file(const std::string& path, Settings& settings, std::string& error);
void load_server_config_file(const std::string& path, unsigned& port);
std::string make_public_url(const std::string& specified_url, unsigned port);
