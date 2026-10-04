#include "mc_play.h"
#include "mc_types.h"
#include "mc_protocol.h"
#include "esp_log.h"
#include "config.h"

static constexpr const char* TAG = "mc_play";

void send_chunk(int sock, PacketBuf& out, ChunkScratch& w, int cx, int cz) {
    world_encode_chunk(out, w, cx, cz);
    out.send_packet(sock);
}

void send_unload_chunk(int sock, PacketBuf& out, int cx, int cz) {
    pkt_begin(out, PlayOut::UnloadChunk);
    pkt_write_i32(out, cz);
    pkt_write_i32(out, cx);
    out.send_packet(sock);
}

void send_center_chunk(int sock, PacketBuf& out, int cx, int cz) {
    pkt_begin(out, PlayOut::SetCenterChunk);
    pkt_write_varint(out, cx);
    pkt_write_varint(out, cz);
    out.send_packet(sock);
}

static void send_login(int sock, PacketBuf& out, int32_t eid) {
    pkt_begin(out, PlayOut::Login);
    pkt_write_i32(out, eid);
    pkt_write_bool(out, false);
    pkt_write_varint(out, 1);
    pkt_write_string(out, "minecraft:overworld");
    pkt_write_varint(out, MC_MAX_PLAYERS);
    pkt_write_varint(out, MC_VIEW_DISTANCE);
    pkt_write_varint(out, MC_SIM_DISTANCE);
    pkt_write_bool(out, false);
    pkt_write_bool(out, true);
    pkt_write_bool(out, false);
    pkt_write_varint(out, 0);
    pkt_write_string(out, "minecraft:overworld");
    pkt_write_i64(out, static_cast<int64_t>(world_seed()));
    pkt_write_byte(out, 1);
    pkt_write_byte(out, 0xFF);
    pkt_write_bool(out, false);
    // isFlat: puts the horizon at the bottom of the world, since our terrain sits far below y=63
    pkt_write_bool(out, true);
    pkt_write_bool(out, false);
    pkt_write_varint(out, 0);
    pkt_write_varint(out, 63);
    pkt_write_bool(out, false);
    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent Login (Play) eid=%d", static_cast<int>(eid));
}

static void send_game_event(int sock, PacketBuf& out) {
    pkt_begin(out, PlayOut::GameEvent);
    pkt_write_byte(out, 13);
    pkt_write_f32(out, 0.0f);
    out.send_packet(sock);
}

void send_play_packets(int sock, PacketBuf& out, ChunkScratch& w, int32_t eid) {
    auto [sx, sy, sz] = world_spawn();
    int scx = sx >> 4, scz = sz >> 4;

    send_login(sock, out, eid);
    send_game_event(sock, out);
    send_center_chunk(sock, out, scx, scz);

    constexpr int vd = MC_VIEW_DISTANCE;
    for (int cx = scx - vd; cx <= scx + vd; cx++)
        for (int cz = scz - vd; cz <= scz + vd; cz++)
            send_chunk(sock, out, w, cx, cz);
    ESP_LOGI(TAG, "Sent %d chunks", (2 * vd + 1) * (2 * vd + 1));

    pkt_begin(out, PlayOut::SpawnPosition);
    pkt_write_position(out, sx, sy, sz);
    pkt_write_f32(out, 0.0f);
    out.send_packet(sock);

    pkt_begin(out, PlayOut::SyncPlayerPosition);
    pkt_write_varint(out, 1);
    pkt_write_f64(out, sx + 0.5);
    pkt_write_f64(out, static_cast<double>(sy));
    pkt_write_f64(out, sz + 0.5);
    pkt_write_f64(out, 0.0);
    pkt_write_f64(out, 0.0);
    pkt_write_f64(out, 0.0);
    pkt_write_f32(out, 0.0f);
    pkt_write_f32(out, 0.0f);
    pkt_write_i32(out, 0);
    out.send_packet(sock);
    ESP_LOGI(TAG, "Spawned at %d %d %d", sx, sy, sz);
}
