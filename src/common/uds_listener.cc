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

#include "common/uds_listener.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <thread>

#include "vesal/log_setting.h"

namespace vesal {

int WriteFd(int fd, const char* buf, size_t count) {
    size_t total_written = 0;
    size_t real_count = count + strlen(VESAL_UDS_LISTENER_MSG_ENDING);
    while (total_written < real_count) {
        int written;
        if (total_written < count) {
            // Still working on real message
            written = send(fd, buf + total_written, count - total_written, MSG_NOSIGNAL);
        } else {
            // Working on ending message
            written = send(fd,
                           &VESAL_UDS_LISTENER_MSG_ENDING[0] + total_written - count,
                           strlen(VESAL_UDS_LISTENER_MSG_ENDING) - (total_written - count),
                           MSG_NOSIGNAL);
        }
        if (written < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            VESAL_LOG(ERROR) << "write failed, errno=" << errno;
            return -1;
        }
        total_written += written;
    }
    return total_written - strlen(VESAL_UDS_LISTENER_MSG_ENDING);
}

int ReadFd(int fd, char* buf, size_t buf_len) {
    size_t total_read = 0;
    int ending_len = strlen(VESAL_UDS_LISTENER_MSG_ENDING);
    bool fini_early = false;
    while (total_read < buf_len) {
        int read_n = read(fd, buf + total_read, buf_len - total_read);
        if (read_n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            VESAL_LOG(ERROR) << "read failed, errno=" << errno;
            return -1;
        }
        total_read += read_n;
        if (strncmp(buf + total_read - ending_len, VESAL_UDS_LISTENER_MSG_ENDING, ending_len) ==
            0) {
            fini_early = true;
            break;
        }
        if (read_n == 0) {
            break;
        }
    }
    if (fini_early) {
        total_read -= ending_len;
    }
    return total_read;
}

bool UdsListener::Start() {
    auto r = Prepare();
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Prepare failed, err=" << r.ToString();
        Reset();
        return false;
    }
    t_ = std::thread(&UdsListener::Listen, this);
    return true;
}

void UdsListener::Stop() {
    ssize_t write_n = write(wakeup_fd_[1], "1", 1);
    while (write_n <= 0 && errno == EINTR) {
        write_n = write(wakeup_fd_[1], "1", 1);
    }
    VESAL_CHECK(write_n == 1) << "write failed, errno=" << errno;
    t_.join();
    unlink(socket_path_.c_str());
    Reset();
}

void UdsListener::Listen() {
    struct epoll_event ev;
    while (true) {
        int nfds = epoll_wait(epoll_fd_, events_, kUdsListenerMaxEvents, -1);
        if (nfds < 0) {
            VESAL_LOG(ERROR) << "epoll_wait failed, errno=" << errno;
            return;
        }

        for (int n = 0; n < nfds; ++n) {
            if (events_[n].data.fd == listen_fd_) {
                VESAL_LOG(DEBUG) << "New connection, listen_fd_=" << listen_fd_;
                // New connection
                int connection_fd = accept(listen_fd_, NULL, NULL);
                if (connection_fd < 0) {
                    VESAL_LOG(ERROR) << "accept failed, errno=" << errno;
                    continue;
                }
                ev.events = EPOLLIN;
                ev.data.fd = connection_fd;
                if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, connection_fd, &ev) < 0) {
                    VESAL_LOG(ERROR) << "epoll_ctl failed, errno=" << errno;
                    close(connection_fd);
                    continue;
                }
                conn_fds_.insert(connection_fd);
            } else if (events_[n].data.fd == wakeup_fd_[0]) {
                VESAL_LOG(INFO) << "Closing UdsListener";
                return;
            } else {
                // Data available to read. It must be read in one go.
                char buffer[kUdsListenerMaxBuffSize + 1];
                int fd = events_[n].data.fd;
                ssize_t bytes_read = ReadFd(fd, buffer, kUdsListenerMaxBuffSize);
                VESAL_LOG(DEBUG) << "read fd=" << fd << ", bytes_read=" << bytes_read
                                 << ", expected_msg_len_=" << expected_msg_len_;
                if (bytes_read == expected_msg_len_) {
                    buffer[expected_msg_len_] = '\0';
                    std::string __msg(buffer, expected_msg_len_);
                    std::string response;
                    msg_handler_(__msg, &response);
                    int write_r = WriteFd(fd, response.c_str(), response.size());
                    if (write_r != static_cast<int>(response.size())) {
                        VESAL_LOG(ERROR) << "send failed, errno=" << errno;
                    }
                } else if (bytes_read < 0) {
                    VESAL_LOG(ERROR)
                        << "read failed, errno=" << errno << ", bytes_read=" << bytes_read
                        << ", expected_msg_len_=" << expected_msg_len_;
                }
                // TODO(sjj): Close the connection here is not wise. It should be closed after the
                // client received response(which we don't have yet). Add the response and send it
                // back to client later. Now we keep it like this.
                close(fd);
                conn_fds_.erase(fd);
            }
        }
    }
}

Status UdsListener::Prepare() {
    // Create an epoll instance
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        return ResourceBusyError("epoll_create1 failed errno=" + std::to_string(errno));
    }

    // Create a socket for listening
    struct sockaddr_un sock_addr;
    struct epoll_event ev;
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return ResourceBusyError("create socket failed, errno=" + std::to_string(errno));
    }
    // Unlink any existing socket file
    unlink(socket_path_.c_str());
    // Set up the socket address structure
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sun_family = AF_UNIX;
    strncpy(sock_addr.sun_path, socket_path_.c_str(), sizeof(sock_addr.sun_path) - 1);
    // Bind the socket to the address
    if (bind(listen_fd_, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) < 0) {
        return ResourceBusyError("bind failed errno=" + std::to_string(errno));
    }
    if (listen(listen_fd_, 5) < 0) {
        return ResourceBusyError("listen failed errno=" + std::to_string(errno));
    }
    // Add the listen_fd to the epoll instance
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        return ResourceBusyError("epoll_ctl failed errno=" + std::to_string(errno));
    }
    // Create a pipe for wakeup
    int pipe_r = pipe(wakeup_fd_);
    if (pipe_r < 0) {
        return ResourceBusyError("pipe failed, errno=" + std::to_string(errno));
    }
    // Add the wakeup_fd to the epoll instance
    ev.events = EPOLLIN;
    ev.data.fd = wakeup_fd_[0];
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_[0], &ev) < 0) {
        return ResourceBusyError("epoll_ctl failed errno=" + std::to_string(errno));
    }
    return OkStatus();
}

bool WriteUdsAndReadResponse(const std::string& uds_path,
                             const std::string& msg,
                             std::string* response) {
    if (msg.size() > kUdsListenerMaxBuffSize) {
        VESAL_LOG(ERROR) << "msg too long, msg.size()=" << msg.size() << ", should not exceed "
                         << kUdsListenerMaxBuffSize << "bytes";
        return false;
    }
    ScopedUdsSocket s(uds_path);
    int client_fd = s.GetFd();
    if (client_fd < 0) {
        VESAL_LOG(ERROR) << "create socket failed, errno=" << errno;
        return false;
    }
    int ret = WriteFd(client_fd, msg.c_str(), msg.size());
    VESAL_CHECK(ret == static_cast<int>(msg.size()))
        << "Should send in one go, ret=" << ret << ", msg.size()=" << msg.size();

    if (!response) {
        return true;
    }
    int read_buf_len = 128 * 1024;
    char read_buf[128 * 1024];
    int read_n = ReadFd(client_fd, read_buf, read_buf_len);
    if (read_n < 0) {
        VESAL_LOG(ERROR) << "read failed, errno=" << errno;
        return false;
    }
    *response = std::string(read_buf, read_n);
    return true;
}

}  // namespace vesal
