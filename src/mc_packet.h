#pragma once

#include <cstdint>
#include <cstddef>

struct PacketBuf {
    uint8_t* data = nullptr;
    size_t cap = 0;
    size_t len = 0;
    size_t pos = 0;
    // Set when a read runs past the end or an allocation fails
    bool err = false;

    bool init(size_t initial_cap = 1024);
    void free();
    void reset();
    bool ensure(size_t additional);
    void append(const uint8_t* src, size_t n);
    size_t remaining() const { return len - pos; }

    bool recv_packet(int sock);
    bool send_packet(int sock);
    void frame_into(PacketBuf& dst) const;
};

bool send_all(int sock, const uint8_t* data, size_t n);
