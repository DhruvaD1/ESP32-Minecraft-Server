#include "mc_players.h"
#include "mc_packet.h"
#include "mc_nbt.h"
#include "mc_protocol.h"
#include "mc_world.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>

static constexpr const char* TAG = "mc_players";
static constexpr size_t QUEUE_LIMIT = 64 * 1024;
static constexpr int32_t PLAYER_ENTITY_TYPE = 147;

// A FreeRTOS mutex for std::scoped_lock, and a player table that's built in place and
// never destroyed: std::mutex and a static destructor drag ~7 KB of libstdc++ into flash.
class RtosMutex {
    StaticSemaphore_t storage_;
    SemaphoreHandle_t handle_ = xSemaphoreCreateMutexStatic(&storage_);

public:
    void lock() { xSemaphoreTake(handle_, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(handle_); }
};

struct Player {
    bool used = false, in_play = false, overflow = false;
    int32_t eid = 0;
    Uuid uuid;
    std::array<char, 17> name{};
    Vec3d pos{};
    Rotation rot{};
    bool on_ground = false;
    PacketBuf queue, spare, build;

    std::string_view name_view() const { return name.data(); }
};

// Other tasks only ever append to a player's queue under g_lock. The owning
// task drains it in players_flush, so it's the only one writing to its socket.
using PlayerTable = std::array<Player, MC_MAX_PLAYERS>;
alignas(PlayerTable) static std::byte g_player_storage[sizeof(PlayerTable)];
static PlayerTable& g_players = *std::construct_at(reinterpret_cast<PlayerTable*>(g_player_storage));
static RtosMutex g_lock;
static int32_t g_next_eid = 1;

static uint8_t angle(float deg) {
    return static_cast<uint8_t>(static_cast<int>(floorf(deg * 256.0f / 360.0f)) & 0xFF);
}

static void enqueue(Player& p, const PacketBuf& pkt) {
    if (!p.in_play || p.overflow || pkt.err) return;
    if (p.queue.len + pkt.len + 5 > QUEUE_LIMIT) {
        p.overflow = true;
        return;
    }
    pkt.frame_into(p.queue);
    if (p.queue.err) p.overflow = true;
}

static void broadcast(const PacketBuf& pkt, const Player* except) {
    for (auto& p : g_players)
        if (&p != except) enqueue(p, pkt);
}

static void write_info_add(PacketBuf& b, std::span<const Player* const> list) {
    pkt_begin(b, PlayOut::PlayerInfoUpdate);
    pkt_write_byte(b, 0x01 | 0x04 | 0x08 | 0x10);
    pkt_write_varint(b, static_cast<int32_t>(list.size()));
    for (const Player* p : list) {
        pkt_write_uuid(b, p->uuid);
        pkt_write_string(b, p->name_view());
        pkt_write_varint(b, 0);
        pkt_write_varint(b, 1);
        pkt_write_bool(b, true);
        pkt_write_varint(b, 0);
    }
}

static void write_spawn(PacketBuf& b, const Player& p) {
    pkt_begin(b, PlayOut::SpawnEntity);
    pkt_write_varint(b, p.eid);
    pkt_write_uuid(b, p.uuid);
    pkt_write_varint(b, PLAYER_ENTITY_TYPE);
    pkt_write_f64(b, p.pos.x);
    pkt_write_f64(b, p.pos.y);
    pkt_write_f64(b, p.pos.z);
    pkt_write_byte(b, angle(p.rot.pitch));
    pkt_write_byte(b, angle(p.rot.yaw));
    pkt_write_byte(b, angle(p.rot.yaw));
    pkt_write_varint(b, 0);
    pkt_write_i16(b, 0);
    pkt_write_i16(b, 0);
    pkt_write_i16(b, 0);
}

static void write_head(PacketBuf& b, const Player& p) {
    pkt_begin(b, PlayOut::HeadRotation);
    pkt_write_varint(b, p.eid);
    pkt_write_byte(b, angle(p.rot.yaw));
}

static void write_sync(PacketBuf& b, const Player& p) {
    pkt_begin(b, PlayOut::SyncEntityPosition);
    pkt_write_varint(b, p.eid);
    pkt_write_f64(b, p.pos.x);
    pkt_write_f64(b, p.pos.y);
    pkt_write_f64(b, p.pos.z);
    pkt_write_f64(b, 0.0);
    pkt_write_f64(b, 0.0);
    pkt_write_f64(b, 0.0);
    pkt_write_f32(b, p.rot.yaw);
    pkt_write_f32(b, p.rot.pitch);
    pkt_write_bool(b, p.on_ground);
}

static void write_chat(PacketBuf& b, std::string_view text, std::optional<std::string_view> color = {}) {
    pkt_begin(b, PlayOut::SystemChat);
    nbt_begin(b);
    nbt_string(b, "text", text);
    if (color) nbt_string(b, "color", *color);
    nbt_end(b);
    pkt_write_bool(b, false);
}

// The client decodes NBT strings as Java modified UTF-8, which has no 4-byte
// sequences, so emoji and broken bytes become '?' instead of kicking everyone.
static std::string_view sanitize(std::string_view in, std::span<char> out) {
    size_t o = 0;
    size_t i = 0;
    while (i < in.size() && o + 4 < out.size()) {
        auto c = static_cast<uint8_t>(in[i]);
        size_t n = (c < 0x80) ? 1 : ((c >> 5) == 0x6) ? 2 : ((c >> 4) == 0xE) ? 3 : ((c >> 3) == 0x1E) ? 4 : 0;
        bool ok = n > 0 && i + n <= in.size() && !(n == 1 && c < 0x20);
        for (size_t k = 1; ok && k < n; k++) ok = (static_cast<uint8_t>(in[i + k]) & 0xC0) == 0x80;
        if (ok && n < 4) {
            std::copy_n(in.data() + i, n, out.begin() + o);
            o += n;
        } else {
            out[o++] = '?';
        }
        i += ok ? n : 1;
    }
    return {out.data(), o};
}

std::expected<int, std::string_view> players_join(std::string_view name, Uuid uuid) {
    std::scoped_lock lock(g_lock);
    auto same = [&](const Player& p) {
        return p.used && (p.uuid == uuid ||
                          std::ranges::equal(p.name_view(), name, [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                          }));
    };
    if (std::ranges::any_of(g_players, same)) return std::unexpected("That name is already online");

    auto it = std::ranges::find_if(g_players, [](const Player& p) { return !p.used; });
    if (it == g_players.end()) return std::unexpected("Server is full");

    Player& p = *it;
    for (PacketBuf* buf : {&p.queue, &p.spare, &p.build})
        if (!buf->allocated()) *buf = PacketBuf(4096);
    if (!p.queue.allocated() || !p.spare.allocated() || !p.build.allocated())
        return std::unexpected("Server is out of memory");

    auto [sx, sy, sz] = world_spawn();
    p.used = true;
    p.in_play = false;
    p.overflow = false;
    p.eid = g_next_eid++;
    p.uuid = uuid;
    p.name.fill('\0');
    std::copy_n(name.data(), std::min(name.size(), p.name.size() - 1), p.name.begin());
    p.pos = {sx + 0.5, static_cast<double>(sy), sz + 0.5};
    p.rot = {};
    p.on_ground = true;
    p.queue.reset();
    p.spare.reset();
    return static_cast<int>(it - g_players.begin());
}

int32_t players_eid(int slot) {
    std::scoped_lock lock(g_lock);
    return g_players[slot].eid;
}

void players_enter_play(int slot) {
    std::scoped_lock lock(g_lock);
    Player& me = g_players[slot];
    PacketBuf& b = me.build;
    me.in_play = true;

    const std::array<const Player*, 1> self{&me};
    write_info_add(b, self);
    broadcast(b, &me);
    write_spawn(b, me);
    broadcast(b, &me);

    std::array<const Player*, MC_MAX_PLAYERS> list{};
    size_t n = 0;
    for (const auto& p : g_players)
        if (p.in_play) list[n++] = &p;
    write_info_add(b, std::span(list).first(n));
    enqueue(me, b);

    for (const auto& other : g_players) {
        if (&other == &me || !other.in_play) continue;
        write_spawn(b, other);
        enqueue(me, b);
        write_head(b, other);
        enqueue(me, b);
    }

    char text[64];
    int len = snprintf(text, sizeof(text), "%s joined the game", me.name.data());
    write_chat(b, {text, static_cast<size_t>(len)}, "yellow");
    broadcast(b, nullptr);
    ESP_LOGI(TAG, "%s joined (%d online)", me.name.data(), static_cast<int>(n));
}

void players_leave(int slot) {
    std::scoped_lock lock(g_lock);
    Player& p = g_players[slot];
    if (!p.used) return;
    bool was_playing = p.in_play;
    p.in_play = false;
    p.used = false;
    p.queue.reset();
    if (!was_playing) return;

    PacketBuf& b = p.build;
    pkt_begin(b, PlayOut::RemoveEntities);
    pkt_write_varint(b, 1);
    pkt_write_varint(b, p.eid);
    broadcast(b, &p);

    pkt_begin(b, PlayOut::PlayerInfoRemove);
    pkt_write_varint(b, 1);
    pkt_write_uuid(b, p.uuid);
    broadcast(b, &p);

    char text[64];
    int len = snprintf(text, sizeof(text), "%s left the game", p.name.data());
    write_chat(b, {text, static_cast<size_t>(len)}, "yellow");
    broadcast(b, &p);
    ESP_LOGI(TAG, "%s left", p.name.data());
}

void players_move(int slot, std::optional<Vec3d> pos, std::optional<Rotation> rot, bool on_ground) {
    if (pos && !(std::isfinite(pos->x) && std::isfinite(pos->y) && std::isfinite(pos->z) &&
                 std::fabs(pos->x) <= 3.0e7 && std::fabs(pos->y) <= 3.0e7 && std::fabs(pos->z) <= 3.0e7))
        return;
    if (rot && !(std::isfinite(rot->yaw) && std::isfinite(rot->pitch))) return;

    std::scoped_lock lock(g_lock);
    Player& p = g_players[slot];
    if (!p.in_play) return;
    if (pos) p.pos = *pos;
    if (rot) p.rot = *rot;
    p.on_ground = on_ground;

    write_sync(p.build, p);
    broadcast(p.build, &p);
    if (rot) {
        write_head(p.build, p);
        broadcast(p.build, &p);
    }
}

void players_chat(int slot, std::string_view msg) {
    std::array<char, 512> clean;
    std::string_view text_in = sanitize(msg, clean);

    std::scoped_lock lock(g_lock);
    Player& p = g_players[slot];
    if (!p.in_play) return;
    char text[600];
    int len = snprintf(text, sizeof(text), "<%s> %.*s", p.name.data(), static_cast<int>(text_in.size()), text_in.data());
    std::string_view line(text, std::min<size_t>(len, sizeof(text) - 1));
    write_chat(p.build, line);
    broadcast(p.build, nullptr);
    ESP_LOGI(TAG, "%.*s", static_cast<int>(line.size()), line.data());
}

void players_swing(int slot, int hand) {
    std::scoped_lock lock(g_lock);
    Player& p = g_players[slot];
    if (!p.in_play) return;
    PacketBuf& b = p.build;
    pkt_begin(b, PlayOut::Animation);
    pkt_write_varint(b, p.eid);
    pkt_write_byte(b, hand == 1 ? 3 : 0);
    broadcast(b, &p);
}

bool players_flush(int slot, int sock) {
    Player& p = g_players[slot];
    {
        std::scoped_lock lock(g_lock);
        if (p.overflow) return false;
        if (p.queue.len == 0) return true;
        std::swap(p.queue, p.spare);
        p.queue.reset();
    }
    bool ok = send_all(sock, p.spare.bytes());
    p.spare.reset();
    return ok;
}

int players_online() {
    std::scoped_lock lock(g_lock);
    return static_cast<int>(std::ranges::count_if(g_players, &Player::in_play));
}
