/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The original code is copyright (c) 2026, 1therealcloud.
 */

#include "http.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#   include <windows.h>
#   include <mswsock.h>
#else
#   include <arpa/inet.h>
#   include <fcntl.h>
#   include <pthread.h>
#   include <signal.h>
#   include <sys/sendfile.h>
#   include <sys/stat.h>
#   include <sys/time.h>
#   include <unistd.h>
#endif

namespace
{
#ifdef _WIN32
    const socket_handle invalid_socket = INVALID_SOCKET;
    void close_socket(socket_handle fd)
    {
        closesocket(fd);
    }
    void shutdown_socket(socket_handle fd)
    {
        shutdown(fd, SD_BOTH);
    }
#else
    const socket_handle invalid_socket = -1;
    void close_socket(socket_handle fd)
    {
        close(fd);
    }
    void shutdown_socket(socket_handle fd)
    {
        shutdown(fd, SHUT_RDWR);
    }
#endif

    bool send_all(socket_handle fd, const char* data, std::size_t length)
    {
        while (length)
        {
            const int portion = static_cast<int>(std::min<std::size_t>(length, 65536));
#ifdef _WIN32
            const int n = send(fd, data, portion, 0);
#else
            const int n = send(fd, data, portion, MSG_NOSIGNAL);
#endif
            if (n < 0)
            {
#ifndef _WIN32
                if (errno == EINTR)
                    continue;
#endif
                return false;
            }
            if (n == 0)
                return false;
            data += n;
            length -= static_cast<std::size_t>(n);
        }
        return true;
    }

    void set_timeout(socket_handle fd)
    {
#ifdef _WIN32
        DWORD timeout = 3000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }

    bool send_header(socket_handle fd, unsigned code, const char* status, std::uint32_t length, const char* content_type, bool keep_alive, bool cacheable)
    {
        char header[512];
        const int n = std::snprintf(header, sizeof(header),
                                    "HTTP/1.1 %u %s\r\nContent-Length: %u\r\nContent-Type: %s\r\n"
                                    "Cache-Control: %s\r\nConnection: %s\r\n%s\r\n",
                                    code, status, length, content_type, cacheable ? "public, max-age=31536000, immutable" : "no-store", keep_alive ? "keep-alive" : "close",
                                    keep_alive ? "Keep-Alive: timeout=3, max=16\r\n" : "");
        return n > 0 && n < static_cast<int>(sizeof(header)) && send_all(fd, header, static_cast<std::size_t>(n));
    }

    bool send_error(socket_handle fd, unsigned code, const char* status, const char* text, bool head)
    {
        const std::uint32_t length = static_cast<std::uint32_t>(std::strlen(text));
        return send_header(fd, code, status, length, "text/plain", false, false) && (head || send_all(fd, text, length));
    }

    std::string lower(const std::string& text)
    {
        std::string result(text);
        for (std::size_t i = 0; i < result.size(); ++i)
            if (result[i] >= 'A' && result[i] <= 'Z')
                result[i] = static_cast<char>(result[i] + ('a' - 'A'));
        return result;
    }

    struct HttpRequest
    {
        std::string path;
        bool head;
        bool keep_alive;
        bool valid;
    };

    bool receive_request(socket_handle fd, std::string& pending, HttpRequest& request)
    {
        std::size_t end = pending.find("\r\n\r\n");
        while (end == std::string::npos)
        {
            if (pending.size() >= 4096)
                return false;
            char bytes[2048];
            const int n = recv(fd, bytes, sizeof(bytes), 0);
            if (n <= 0)
                return false;
            pending.append(bytes, n);
            end = pending.find("\r\n\r\n");
        }
        const std::string headers = pending.substr(0, end + 4);
        pending.erase(0, end + 4);
        const std::size_t line_end = headers.find("\r\n");
        if (line_end == std::string::npos || line_end > 2048)
            return false;
        const std::string line = headers.substr(0, line_end);
        const std::size_t a = line.find(' ');
        const std::size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
        if (a == std::string::npos || b == std::string::npos || line.find(' ', b + 1) != std::string::npos)
            return false;
        const std::string method = line.substr(0, a);
        const std::string version = line.substr(b + 1);
        request.head = method == "HEAD";
        request.path = line.substr(a + 1, b - a - 1);
        request.valid = (method == "GET" || request.head) && (version == "HTTP/1.0" || version == "HTTP/1.1") && !request.path.empty() && request.path[0] == '/';
        request.keep_alive = version == "HTTP/1.1";
        std::size_t pos = line_end + 2;
        while (pos < end)
        {
            const std::size_t next = headers.find("\r\n", pos);
            if (next == std::string::npos || next > end)
                return false;
            const std::string field = lower(headers.substr(pos, next - pos));
            if (field.find("connection:") == 0)
            {
                const std::string value = field.substr(11);
                if (value.find("close") != std::string::npos)
                    request.keep_alive = false;
                else if (value.find("keep-alive") != std::string::npos)
                    request.keep_alive = true;
            }
            // We don't support incoming request bodies or pipelined uploads.
            if (field.find("content-length:") == 0 || field.find("transfer-encoding:") == 0)
                request.valid = false;
            pos = next + 2;
        }
        return true;
    }
}

struct CHttpServer::OpenFile
{
#ifdef _WIN32
    HANDLE fd;
    OpenFile() : fd(INVALID_HANDLE_VALUE)
    {}
    ~OpenFile()
    {
        if (fd != INVALID_HANDLE_VALUE)
            CloseHandle(fd);
    }
#else
    int fd;
    OpenFile() : fd(-1)
    {}
    ~OpenFile()
    {
        if (fd >= 0)
            close(fd);
    }
#endif
};

CHttpServer::CHttpServer(Assets& assets)
    : assets_(assets), listen_(invalid_socket), running_(false), waiting_(0), max_workers_(16)
#ifdef _WIN32
      ,
      winsock_started_(false)
#endif
{}

CHttpServer::~CHttpServer()
{
    stop();
}

bool CHttpServer::open_file(const Asset& asset, OpenFile& file)
{
#ifdef _WIN32
    file.fd = CreateFileA(asset.filename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (file.fd == INVALID_HANDLE_VALUE)
        return false;
    FileStamp now;
    return file_stamp_native(reinterpret_cast<std::intptr_t>(file.fd), now) && now == asset.stamp && now.size == asset.size;
#else
    file.fd = open(asset.filename.c_str(), O_RDONLY | O_CLOEXEC);
    if (file.fd < 0)
        return false;
    FileStamp now;
    return file_stamp_native(file.fd, now) && now == asset.stamp && now.size == asset.size;
#endif
}

bool CHttpServer::transmit(socket_handle fd, const OpenFile& file, std::uint32_t size)
{
#ifdef _WIN32
    return TransmitFile(fd, file.fd, size, 0, 0, 0, 0) != FALSE;
#else
    off_t offset = 0;
    while (offset < static_cast<off_t>(size))
    {
        const ssize_t n = sendfile(fd, file.fd, &offset, static_cast<std::size_t>(size - offset));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
    }
    return true;
#endif
}

bool CHttpServer::start(const std::string& bind_address, std::uint16_t port, std::string& error)
{
    if (running_)
    {
        error = "already running";
        return false;
    }
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        error = "WSAStartup failed";
        return false;
    }
    winsock_started_ = true;
#endif
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == invalid_socket)
    {
        error = "socket failed";
        stop();
        return false;
    }
    int reuse = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1)
    {
        error = "bind must be a numeric IPv4 address";
        stop();
        return false;
    }
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_, 64) != 0)
    {
        error = "TCP bind/listen failed";
        stop();
        return false;
    }
    running_ = true;
    try
    {
        const unsigned cpus = std::thread::hardware_concurrency();
        max_workers_ = (std::max)(4u, (std::min)(32u, cpus ? cpus * 2 : 8u));
        workers_.reserve(max_workers_);
        for (unsigned i = 0; i < 2; ++i)
            workers_.push_back(std::thread(&CHttpServer::work_loop, this));
        accept_thread_ = std::thread(&CHttpServer::accept_loop, this);
    }
    catch (...)
    {
        error = "could not start HTTP threads";
        stop();
        return false;
    }
    return true;
}

void CHttpServer::stop()
{
    running_ = false;
    ready_.notify_all();
    if (listen_ != invalid_socket)
        shutdown_socket(listen_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::set<socket_handle>::const_iterator it = active_.begin(); it != active_.end(); ++it)
            shutdown_socket(*it); // wake workers blocked in recv/sendfile
        while (!queue_.empty())
        {
            close_socket(queue_.front());
            queue_.pop();
        }
    }
    if (accept_thread_.joinable())
        accept_thread_.join();
    if (listen_ != invalid_socket)
    {
        close_socket(listen_);
        listen_ = invalid_socket;
    }
    for (std::size_t i = 0; i < workers_.size(); ++i)
        if (workers_[i].joinable())
            workers_[i].join();
    workers_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty())
        {
            close_socket(queue_.front());
            queue_.pop();
        }
    }
#ifdef _WIN32
    if (winsock_started_)
    {
        WSACleanup();
        winsock_started_ = false;
    }
#endif
}

void CHttpServer::accept_loop()
{
    while (running_)
    {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listen_, &read_set);
        timeval timeout = {1, 0};
#ifdef _WIN32
        const int selected = select(0, &read_set, 0, 0, &timeout);
#else
        const int selected = select(listen_ + 1, &read_set, 0, 0, &timeout);
#endif
        if (!running_)
            break;
        if (selected <= 0)
            continue;
        const socket_handle fd = accept(listen_, 0, 0);
        if (fd == invalid_socket)
            continue;
        set_timeout(fd);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || queue_.size() >= 128)
            {
                close_socket(fd);
                continue;
            }
            queue_.push(fd);
            if (waiting_ < queue_.size() && workers_.size() < max_workers_)
            {
                try
                {
                    workers_.push_back(std::thread(&CHttpServer::work_loop, this));
                }
                catch (...)
                { /* existing workers still handle queued connections */
                }
            }
        }
        ready_.notify_one();
    }
}

void CHttpServer::work_loop()
{
#ifndef _WIN32
    // sendfile() can raise SIGPIPE; block it only in HTTP workers.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, 0);
#endif
    for (;;)
    {
        socket_handle fd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++waiting_;
            ready_.wait(lock, [this] {
                return !running_ || !queue_.empty();
            });
            --waiting_;
            if (!running_)
                return;
            fd = queue_.front();
            queue_.pop();
            active_.insert(fd);
        }
        serve(fd);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.erase(fd);
            close_socket(fd);
        }
    }
}

void CHttpServer::serve(socket_handle fd)
{
    std::string pending;
    for (unsigned i = 0; i < 16 && running_; ++i)
    {
        HttpRequest request = {};
        if (!receive_request(fd, pending, request))
            return;
        if (!request.valid)
        {
            send_error(fd, 400, "Bad Request", "bad request\n", false);
            return;
        }
        Asset asset;
        if (!assets_.lookup_url(request.path, asset))
        {
            send_error(fd, 404, "Not Found", "not found\n", request.head);
            return;
        }
        OpenFile file;
        if (!open_file(asset, file))
        {
            send_error(fd, 503, "Service Unavailable", "file changed or missing\n", request.head);
            return;
        }
        const bool keep = request.keep_alive && (i + 1 < 16);
        if (!send_header(fd, 200, "OK", asset.size, "application/octet-stream", keep, true))
            return;
        if (!request.head && !transmit(fd, file, asset.size))
        {
            return;
        }
        if (!keep)
            return;
    }
}
