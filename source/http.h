/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#pragma once
#include "artwork.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#   include <winsock2.h>
#   include <ws2tcpip.h>
typedef SOCKET socket_handle;
#else
#   include <sys/socket.h>
#   include <netinet/in.h>
typedef int socket_handle;
#endif

class CHttpServer
{
public:
    explicit CHttpServer(Assets& assets);
    ~CHttpServer();
    bool start(const std::string& bind_address, std::uint16_t port, std::string& error);
    void stop();

private:
    struct OpenFile;
    void accept_loop();
    void work_loop();
    void serve(socket_handle fd);
    bool open_file(const Asset& asset, OpenFile& file);
    bool transmit(socket_handle fd, const OpenFile& file, std::uint32_t size);

    Assets& assets_;
    socket_handle listen_;
    std::atomic<bool> running_;
    std::thread accept_thread_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<socket_handle> queue_;
    std::set<socket_handle> active_;
    unsigned waiting_;
    unsigned max_workers_;

#ifdef _WIN32
    bool winsock_started_;
#endif
};
