#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include "esp_heap_caps.h"

struct PsramFree {
    void operator()(void* p) const noexcept { heap_caps_free(p); }
};

template <typename T>
using psram_ptr = std::unique_ptr<T[], PsramFree>;

template <typename T>
psram_ptr<T> psram_alloc(size_t count) {
    return psram_ptr<T>(static_cast<T*>(heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM)));
}

class PacketBuf {
    psram_ptr<uint8_t> buf_;
    size_t cap_ = 0;

public:
    size_t len = 0;
    size_t pos = 0;
    // Set when a read runs past the end or an allocation fails
    bool err = false;

    PacketBuf() = default;
    explicit PacketBuf(size_t initial_cap)
        : buf_(psram_alloc<uint8_t>(initial_cap)), cap_(buf_ ? initial_cap : 0), err(!buf_) {}

    PacketBuf(PacketBuf&& o) noexcept
        : buf_(std::move(o.buf_)), cap_(std::exchange(o.cap_, 0)), len(std::exchange(o.len, 0)),
          pos(std::exchange(o.pos, 0)), err(std::exchange(o.err, false)) {}
    PacketBuf& operator=(PacketBuf&& o) noexcept {
        len = std::exchange(o.len, 0);
        pos = std::exchange(o.pos, 0);
        err = std::exchange(o.err, false);
        buf_ = std::move(o.buf_);
        cap_ = std::exchange(o.cap_, 0);
        return *this;
    }
    PacketBuf(const PacketBuf&) = delete;
    PacketBuf& operator=(const PacketBuf&) = delete;

    bool allocated() const { return buf_ != nullptr; }
    uint8_t* data() { return buf_.get(); }
    std::span<const uint8_t> bytes() const { return {buf_.get(), len}; }
    std::span<const uint8_t> unread() const { return bytes().subspan(pos); }

    void reset() {
        len = 0;
        pos = 0;
        err = false;
    }
    bool ensure(size_t additional);
    void append(std::span<const uint8_t> src);

    [[nodiscard]] bool recv_packet(int sock);
    bool send_packet(int sock);
    void frame_into(PacketBuf& dst) const;
};

bool send_all(int sock, std::span<const uint8_t> data);
