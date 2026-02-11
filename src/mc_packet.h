#pragma once

#include <cstdint>
#include <cstddef>

struct PacketBuf {
    uint8_t* data;
    size_t cap;
    size_t len;
    size_t pos;

    void init(size_t initial_cap = 1024);
    void free();
    void reset();
    void ensure(size_t additional);
    void append(const uint8_t* src, size_t n);
    size_t remaining() const { return len - pos; }

    bool recv_packet(int sock);
    bool send_packet(int sock);
};
