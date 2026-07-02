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

#include <fcntl.h>
#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include "vesal/vesal.h"

TEST(UdsListenerTest, StartStop) {
    std::string socket_path = "/tmp/test.sock";
    std::string received;
    std::string message = "1145141919810";
    vesal::UdsListener listener(socket_path,
                                [&received](const std::string& s, void*) {
                                    received = s;
                                    return vesal::OkStatus();
                                },
                                message.size());

    ASSERT_TRUE(listener.Start());
    EXPECT_TRUE(vesal::WriteUdsAndReadResponse(socket_path, message, nullptr));
    usleep(500);
    listener.Stop();
    ASSERT_EQ(received, message);
}

// Only kUdsListenerMaxBuffSize bytes shall be read.
TEST(UdsListenerTest, TooLargeMessage) {
    std::string socket_path = "/tmp/test.sock";
    std::string received;
    std::string message(vesal::kUdsListenerMaxBuffSize * 2, 'a');
    EXPECT_DEATH(vesal::UdsListener listener(socket_path,
                                             [&received](const std::string& s, void*) {
                                                 received = s;
                                                 return vesal::OkStatus();
                                             },
                                             message.size()),
                 ".*");
    message.resize(vesal::kUdsListenerMaxBuffSize);
    vesal::UdsListener listener(socket_path,
                                [&received](const std::string& s, void*) {
                                    received = s;
                                    return vesal::OkStatus();
                                },
                                message.size());
    ASSERT_TRUE(listener.Start());

    EXPECT_TRUE(vesal::WriteUdsAndReadResponse(socket_path, message, nullptr));
    message.resize(vesal::kUdsListenerMaxBuffSize * 2);
    EXPECT_FALSE(vesal::WriteUdsAndReadResponse(socket_path, message, nullptr));
    listener.Stop();
}

// Sticky packet shall be invalid in our UdsListener.
TEST(UdsListenerTest, StickyPacket) {
    std::string socket_path = "/tmp/test.sock";
    std::string received;
    std::string message = "114514";
    vesal::UdsListener listener(socket_path,
                                [&received](const std::string& s, void*) {
                                    received = s;
                                    return vesal::OkStatus();
                                },
                                message.size() * 2);
    ASSERT_TRUE(listener.Start());
    int client_fd;
    client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_GE(client_fd, 0);
    struct sockaddr_un sock_addr;
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sun_family = AF_UNIX;
    strncpy(sock_addr.sun_path, socket_path.c_str(), sizeof(sock_addr.sun_path) - 1);
    int connect_r = connect(client_fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
    EXPECT_EQ(connect_r, 0);
    ssize_t write_n = write(client_fd, message.c_str(), message.size());
    EXPECT_EQ(write_n, message.size()) << "errno: " << errno;
    usleep(500);
    EXPECT_EQ(fcntl(client_fd, F_GETFD), 0);
    EXPECT_EQ(errno, ENOENT);
    close(client_fd);
    listener.Stop();
    EXPECT_EQ(received, "");
}

TEST(ScopedUdsSocketTest, RAII) {
    {
        vesal::ScopedUdsSocket socket("/tmp/test.sock");
        EXPECT_EQ(socket.GetFd(), -1);
    }
    std::string socket_path = "/tmp/test.sock";
    std::string received;
    std::string message = "114514";
    vesal::UdsListener listener(socket_path,
                                [&received](const std::string& s, void*) {
                                    received = s;
                                    return vesal::OkStatus();
                                },
                                message.size());
    ASSERT_TRUE(listener.Start());
    vesal::ScopedUdsSocket socket(socket_path);
    EXPECT_GE(socket.GetFd(), 0);
    listener.Stop();
}

TEST(UdsListenerTest, Response) {
    FLAGS_vesal_log_console_output = true;
    std::string socket_path = "/tmp/test.sock";
    std::string message = "114514";
    std::string response;
    vesal::UdsListener listener(socket_path,
                                [](const std::string& s, void* args) {
                                    std::string* resp = static_cast<std::string*>(args);
                                    *resp = "OK";
                                    return vesal::OkStatus();
                                },
                                message.size());
    ASSERT_TRUE(listener.Start());
    vesal::ScopedUdsSocket socket(socket_path);
    EXPECT_GE(socket.GetFd(), 0);

    EXPECT_TRUE(vesal::WriteUdsAndReadResponse(socket_path, message, &response));
    EXPECT_EQ(response, "OK");
    listener.Stop();
}
