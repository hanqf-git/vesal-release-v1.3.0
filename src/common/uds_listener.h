/*
 * Copyright (c) 2024 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <functional>
#include <thread>
#include <unordered_set>

#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {

#define VESAL_UDS_LISTENER_MSG_ENDING "\x11\x45\x14"

static const size_t kUdsListenerMaxEvents = 10;
static const size_t kUdsListenerMaxBuffSize = 1024;

// A simple listener based on epoll and Unix Domain Socket, enough for our usage. The listener will
// continue to listen the message sent from client (typically another process) and pass it to user
// defined handler. It assumes the message is sent in a single write with a fixed, short length
// (less than 1024B) and should be read out with a single read() syscall.
class UdsListener {
public:
    UdsListener(const std::string& socket_path,
                std::function<Status(const std::string&, std::string*)> msg_handler,
                size_t expected_msg_len)
        : socket_path_(socket_path),
          msg_handler_(msg_handler),
          expected_msg_len_(expected_msg_len),
          listen_fd_(-1),
          epoll_fd_(-1) {
        VESAL_CHECK(expected_msg_len_ <= static_cast<ssize_t>(kUdsListenerMaxBuffSize))
            << "UdsListener is intent to receive short message not longer than "
            << kUdsListenerMaxBuffSize;
    }
    ~UdsListener() {
        Reset();
    }

    bool Start();
    void Stop();

private:
    Status Prepare();
    void Listen();
    void Reset() {
        close(wakeup_fd_[0]);
        close(wakeup_fd_[1]);
        close(listen_fd_);
        close(epoll_fd_);
        for (int fd : conn_fds_) {
            close(fd);
        }
        conn_fds_.clear();
    }

    std::string socket_path_;
    std::function<Status(const std::string&, std::string*)> msg_handler_;
    ssize_t expected_msg_len_;
    int wakeup_fd_[2];

    int listen_fd_;                                     // listen for new connection
    int epoll_fd_;                                      // epoll for new connection and data
    std::unordered_set<int> conn_fds_;                  // connection fd, read data from these fds
    struct epoll_event events_[kUdsListenerMaxEvents];  // epoll events used by epoll_fd_
    std::thread t_;                                     // thread to listen continuously
};

// Write until the whole msg is written.
// The message written will be added by VESAL_UDS_LISTENER_MSG_ENDING.
// Return the number of bytes written execpt the ending. -1 if failed.
// Should be used with ReadFd.
int WriteFd(int fd, const char* buf, size_t count);
// Read until the whole msg is read OR the buf is full.
// Assume the message shall be cut off at VESAL_UDS_LISTENER_MSG_ENDING.
// Return the number of bytes read. -1 if failed.
// Should be used with WriteFd.
int ReadFd(int fd, const char* buf, size_t buf_len);

// Connect the socket, write the msg, read the response, and close the connection.
bool WriteUdsAndReadResponse(const std::string& uds_path,
                             const std::string& msg,
                             std::string* response);

class ScopedUdsSocket {
public:
    ScopedUdsSocket(const std::string& uds_path) : fd(-1) {
        int success_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (success_fd < 0) {
            VESAL_LOG(ERROR) << "create socket failed, errno=" << errno;
            return;
        }
        struct sockaddr_un sock_addr;
        memset(&sock_addr, 0, sizeof(sock_addr));
        sock_addr.sun_family = AF_UNIX;
        strncpy(sock_addr.sun_path, uds_path.c_str(), sizeof(sock_addr.sun_path) - 1);
        int connect_r = connect(success_fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
        if (connect_r < 0) {
            VESAL_LOG(ERROR) << "connect failed, errno=" << errno;
            return;
        }
        fd = success_fd;
    }

    ~ScopedUdsSocket() {
        close(fd);
    }

    int GetFd() const {
        return fd;
    }

private:
    int fd;
};

}  // namespace vesal
