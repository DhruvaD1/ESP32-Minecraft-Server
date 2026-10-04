#include "mc_players.h"
#include "mc_packet.h"
#include "mc_types.h"
#include "mc_nbt.h"
#include "mc_world.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <utility>

static const char* TAG = "mc_players";
static constexpr size_t QUEUE_LIMIT = 64 * 1024;
static constexpr int PLAYER_ENTITY_TYPE = 147;

struct Player {
    bool used, in_play, overflow;
    int32_t eid;
    uint64_t uuid_hi, uuid_lo;
    char name[17];
    double x, y, z;
    float yaw, pitch;
    bool on_ground;
    PacketBuf queue, spare, build;
};

// Other tasks only ever append to a player's queue under g_lock. The owning
// task drains it in players_flush, so it's the only one writing to its socket.
static Player g_players[MC_MAX_PLAYERS];
static SemaphoreHandle_t g_lock;
static int32_t g_next_eid = 1;

struct Guard {
    Guard() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(g_lock); }
};

void players_init() {
    g_lock = xSemaphoreCreateMutex();
}

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

static void broadcast(const PacketBuf& pkt, int except) {
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (i != except) enqueue(g_players[i], pkt);
}

static void write_info_add(PacketBuf& b, const Player* const* list, int n) {
    b.reset();
    pkt_write_varint(b, 0x40);
    pkt_write_byte(b, 0x01 | 0x04 | 0x08 | 0x10);
    pkt_write_varint(b, n);
    for (int i = 0; i < n; i++) {
        pkt_write_uuid(b, list[i]->uuid_hi, list[i]->uuid_lo);
        pkt_write_string(b, list[i]->name);
        pkt_write_varint(b, 0);
        pkt_write_varint(b, 1);
        pkt_write_bool(b, true);
        pkt_write_varint(b, 0);
    }
}

static void write_spawn(PacketBuf& b, const Player& p) {
    b.reset();
    pkt_write_varint(b, 0x01);
    pkt_write_varint(b, p.eid);
    pkt_write_uuid(b, p.uuid_hi, p.uuid_lo);
    pkt_write_varint(b, PLAYER_ENTITY_TYPE);
    pkt_write_f64(b, p.x);
    pkt_write_f64(b, p.y);
    pkt_write_f64(b, p.z);
    pkt_write_byte(b, angle(p.pitch));
    pkt_write_byte(b, angle(p.yaw));
    pkt_write_byte(b, angle(p.yaw));
    pkt_write_varint(b, 0);
    pkt_write_i16(b, 0);
    pkt_write_i16(b, 0);
    pkt_write_i16(b, 0);
}

static void write_head(PacketBuf& b, const Player& p) {
    b.reset();
    pkt_write_varint(b, 0x4D);
    pkt_write_varint(b, p.eid);
    pkt_write_byte(b, angle(p.yaw));
}

static void write_sync(PacketBuf& b, const Player& p) {
    b.reset();
    pkt_write_varint(b, 0x20);
    pkt_write_varint(b, p.eid);
    pkt_write_f64(b, p.x);
    pkt_write_f64(b, p.y);
    pkt_write_f64(b, p.z);
    pkt_write_f64(b, 0.0);
    pkt_write_f64(b, 0.0);
    pkt_write_f64(b, 0.0);
    pkt_write_f32(b, p.yaw);
    pkt_write_f32(b, p.pitch);
    pkt_write_bool(b, p.on_ground);
}

static void write_chat(PacketBuf& b, const char* text, const char* color) {
    b.reset();
    pkt_write_varint(b, 0x73);
    nbt_begin(b);
    nbt_string(b, "text", text);
    if (color) nbt_string(b, "color", color);
    nbt_end(b);
    pkt_write_bool(b, false);
}

// The client decodes NBT strings as Java modified UTF-8, which has no 4-byte
// sequences, so emoji and broken bytes become '?' instead of kicking everyone.
static void sanitize(const char* in, char* out, size_t cap) {
    const auto* s = reinterpret_cast<const uint8_t*>(in);
    size_t o = 0;
    while (*s && o + 4 < cap) {
        uint8_t c = *s;
        int n = (c < 0x80) ? 1 : ((c >> 5) == 0x6) ? 2 : ((c >> 4) == 0xE) ? 3 : ((c >> 3) == 0x1E) ? 4 : 0;
        bool ok = n > 0 && !(n == 1 && c < 0x20);
        for (int k = 1; ok && k < n; k++) ok = (s[k] & 0xC0) == 0x80;
        if (ok && n < 4) {
            memcpy(out + o, s, n);
            o += n;
        } else {
            out[o++] = '?';
        }
        s += ok ? n : 1;
    }
    out[o] = '\0';
}

int players_join(const char* name, uint64_t uuid_hi, uint64_t uuid_lo, const char*& reason) {
    Guard g;
    int slot = -1;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        const Player& p = g_players[i];
        if (!p.used) {
            if (slot < 0) slot = i;
            continue;
        }
        if (strcasecmp(p.name, name) == 0 || (p.uuid_hi == uuid_hi && p.uuid_lo == uuid_lo)) {
            reason = "That name is already online";
            return -1;
        }
    }
    if (slot < 0) {
        reason = "Server is full";
        return -1;
    }

    Player& p = g_players[slot];
    if ((!p.queue.data && !p.queue.init(4096)) ||
        (!p.spare.data && !p.spare.init(4096)) ||
        (!p.build.data && !p.build.init(512))) {
        reason = "Server is out of memory";
        return -1;
    }

    int sx, sy, sz;
    world_spawn(sx, sy, sz);
    p.used = true;
    p.in_play = false;
    p.overflow = false;
    p.eid = g_next_eid++;
    p.uuid_hi = uuid_hi;
    p.uuid_lo = uuid_lo;
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
    p.x = sx + 0.5;
    p.y = sy;
    p.z = sz + 0.5;
    p.yaw = p.pitch = 0.0f;
    p.on_ground = true;
    p.queue.reset();
    p.spare.reset();
    return slot;
}

int32_t players_eid(int slot) {
    Guard g;
    return g_players[slot].eid;
}

void players_enter_play(int slot) {
    Guard g;
    Player& me = g_players[slot];
    PacketBuf& b = me.build;
    me.in_play = true;

    const Player* self[1] = {&me};
    write_info_add(b, self, 1);
    broadcast(b, slot);
    write_spawn(b, me);
    broadcast(b, slot);

    const Player* list[MC_MAX_PLAYERS];
    int n = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++)
        if (g_players[i].in_play) list[n++] = &g_players[i];
    write_info_add(b, list, n);
    enqueue(me, b);

    for (int i = 0; i < MC_MAX_PLAYERS; i++) {
        if (i == slot || !g_players[i].in_play) continue;
        write_spawn(b, g_players[i]);
        enqueue(me, b);
        write_head(b, g_players[i]);
        enqueue(me, b);
    }

    char text[64];
    snprintf(text, sizeof(text), "%s joined the game", me.name);
    write_chat(b, text, "yellow");
    broadcast(b, -1);
    ESP_LOGI(TAG, "%s joined (%d online)", me.name, n);
}

void players_leave(int slot) {
    Guard g;
    Player& p = g_players[slot];
    if (!p.used) return;
    bool was_playing = p.in_play;
    p.in_play = false;
    p.used = false;
    p.queue.reset();
    if (!was_playing) return;

    PacketBuf& b = p.build;
    b.reset();
    pkt_write_varint(b, 0x47);
    pkt_write_varint(b, 1);
    pkt_write_varint(b, p.eid);
    broadcast(b, slot);

    b.reset();
    pkt_write_varint(b, 0x3F);
    pkt_write_varint(b, 1);
    pkt_write_uuid(b, p.uuid_hi, p.uuid_lo);
    broadcast(b, slot);

    char text[64];
    snprintf(text, sizeof(text), "%s left the game", p.name);
    write_chat(b, text, "yellow");
    broadcast(b, slot);
    ESP_LOGI(TAG, "%s left", p.name);
}

void players_move(int slot, const double* pos, const float* rot, bool on_ground) {
    Guard g;
    Player& p = g_players[slot];
    if (!p.in_play) return;

    if (pos) {
        for (int i = 0; i < 3; i++)
            if (!std::isfinite(pos[i]) || fabs(pos[i]) > 3.0e7) return;
        p.x = pos[0];
        p.y = pos[1];
        p.z = pos[2];
    }
    if (rot) {
        if (!std::isfinite(rot[0]) || !std::isfinite(rot[1])) return;
        p.yaw = rot[0];
        p.pitch = rot[1];
    }
    p.on_ground = on_ground;

    write_sync(p.build, p);
    broadcast(p.build, slot);
    if (rot) {
        write_head(p.build, p);
        broadcast(p.build, slot);
    }
}

void players_chat(int slot, const char* msg) {
    char clean[512];
    sanitize(msg, clean, sizeof(clean));

    Guard g;
    Player& p = g_players[slot];
    if (!p.in_play) return;
    char text[600];
    snprintf(text, sizeof(text), "<%s> %s", p.name, clean);
    write_chat(p.build, text, nullptr);
    broadcast(p.build, -1);
    ESP_LOGI(TAG, "%s", text);
}

void players_swing(int slot, int hand) {
    Guard g;
    Player& p = g_players[slot];
    if (!p.in_play) return;
    PacketBuf& b = p.build;
    b.reset();
    pkt_write_varint(b, 0x03);
    pkt_write_varint(b, p.eid);
    pkt_write_byte(b, hand == 1 ? 3 : 0);
    broadcast(b, slot);
}

bool players_flush(int slot, int sock) {
    Player& p = g_players[slot];
    {
        Guard g;
        if (p.overflow) return false;
        if (p.queue.len == 0) return true;
        std::swap(p.queue, p.spare);
        p.queue.reset();
    }
    bool ok = send_all(sock, p.spare.data, p.spare.len);
    p.spare.reset();
    return ok;
}

int players_online() {
    Guard g;
    int n = 0;
    for (int i = 0; i < MC_MAX_PLAYERS; i++) n += g_players[i].in_play;
    return n;
}
