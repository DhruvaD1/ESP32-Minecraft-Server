#include "mc_play.h"
#include "mc_types.h"
#include "mc_nbt.h"
#include "esp_log.h"
#include "config.h"
#include <cmath>
#include <cstring>

static const char* TAG = "mc_play";

static constexpr int SEA_LEVEL = -52;
static constexpr int MIN_Y = -64;
static constexpr int NUM_SECTIONS = 24;

static constexpr int S_AIR   = 0;
static constexpr int S_STONE = 1;
static constexpr int S_GRASS = 8;
static constexpr int S_DIRT  = 10;
static constexpr int S_SAND  = 118;
static constexpr int S_WATER = 86;
static constexpr int S_LOG   = 137;
static constexpr int S_LEAF  = 254;
static constexpr int S_TALLGRASS = 2048;

static constexpr int PI_AIR   = 0;
static constexpr int PI_STONE = 1;
static constexpr int PI_DIRT  = 2;
static constexpr int PI_GRASS = 3;
static constexpr int PI_WATER = 4;
static constexpr int PI_LOG   = 5;
static constexpr int PI_LEAF  = 6;
static constexpr int PI_TALLGRASS = 7;
static constexpr int PI_SAND  = 8;

static const int PALETTE[] = { S_AIR, S_STONE, S_DIRT, S_GRASS, S_WATER, S_LOG, S_LEAF, S_TALLGRASS, S_SAND };
static constexpr int PALETTE_SIZE = 9;

// 0=ocean, 1=plains, 2=mountains
static int biome_at(int bx, int bz) {
    float x = static_cast<float>(bx);
    float z = static_cast<float>(bz);
    float v = sinf(x * 0.005f + 1.3f) * cosf(z * 0.007f + 0.7f)
            + sinf(x * 0.003f - z * 0.004f) * 0.6f;
    if (v < -0.3f) return 0;
    if (v > 0.5f) return 2;
    return 1;
}

static int terrain_height(int bx, int bz) {
    float x = static_cast<float>(bx);
    float z = static_cast<float>(bz);
    float detail = sinf(x * 0.05f) * cosf(z * 0.07f) * 6.0f
                 + sinf(x * 0.13f + z * 0.11f) * 3.0f
                 + cosf(x * 0.21f) * sinf(z * 0.19f) * 1.5f;

    int biome = biome_at(bx, bz);
    int height;
    if (biome == 0) {
        height = -58 + static_cast<int>(detail * 0.4f);
        if (height < -62) height = -62;
        if (height > -54) height = -54;
    } else if (biome == 2) {
        height = -38 + static_cast<int>(detail * 1.5f);
        if (height < -48) height = -48;
        if (height > -28) height = -28;
    } else {
        height = -50 + static_cast<int>(detail * 0.8f);
        if (height < -56) height = -56;
        if (height > -44) height = -44;
    }
    return height;
}

static uint32_t hash_pos(int x, int z) {
    uint32_t h = static_cast<uint32_t>(x * 374761393 + z * 668265263);
    h = (h ^ (h >> 13)) * 1274126177;
    return h ^ (h >> 16);
}

static bool has_tree(int x, int z) {
    return (hash_pos(x, z) & 0x3F) == 0;
}

static bool has_tallgrass(int x, int z) {
    return (hash_pos(x * 7 + 3, z * 11 + 7) & 0x3) < 2;
}

static int get_tree_pi(int dx, int dy, int dz) {
    if (dx == 0 && dz == 0 && dy >= 0 && dy <= 3) return PI_LOG;
    if ((dy == 2 || dy == 3) && abs(dx) <= 2 && abs(dz) <= 2) {
        if (abs(dx) == 2 && abs(dz) == 2) return -1;
        if (dx == 0 && dz == 0) return -1;
        return PI_LEAF;
    }
    if (dy == 4 && abs(dx) <= 1 && abs(dz) <= 1) return PI_LEAF;
    return -1;
}

static int max_tree_dy(int dx, int dz) {
    if (dx == 0 && dz == 0) return 3;
    if (abs(dx) <= 1 && abs(dz) <= 1) return 4;
    if (abs(dx) <= 2 && abs(dz) <= 2 && !(abs(dx) == 2 && abs(dz) == 2)) return 3;
    return -1;
}

struct TreeInfo { int bx, bz, ground; };

static int find_trees(int cx, int cz, TreeInfo* trees, int max_trees) {
    int count = 0;
    for (int bx = cx * 16 - 3; bx < cx * 16 + 19 && count < max_trees; bx++)
        for (int bz = cz * 16 - 3; bz < cz * 16 + 19 && count < max_trees; bz++)
            if (has_tree(bx, bz)) {
                int h = terrain_height(bx, bz);
                if (h >= SEA_LEVEL + 3 && biome_at(bx, bz) != 0)
                    trees[count++] = {bx, bz, h};
            }
    return count;
}

static int get_block(int wx, int wy, int wz, int terrain_h, TreeInfo* trees, int tcnt) {
    for (int t = 0; t < tcnt; t++) {
        int pi = get_tree_pi(wx - trees[t].bx, wy - (trees[t].ground + 1), wz - trees[t].bz);
        if (pi >= 0) return pi;
    }
    if (wy > terrain_h) {
        int biome = biome_at(wx, wz);
        if (wy == terrain_h + 1 && terrain_h >= SEA_LEVEL + 3 && biome == 1 && has_tallgrass(wx, wz))
            return PI_TALLGRASS;
        if (wy <= SEA_LEVEL && terrain_h < SEA_LEVEL) return PI_WATER;
        return PI_AIR;
    }
    int biome = biome_at(wx, wz);
    bool beach = (terrain_h >= SEA_LEVEL && terrain_h <= SEA_LEVEL + 2);
    if (wy == terrain_h) {
        if (beach) return PI_SAND;
        if (biome == 2 && terrain_h > -38) return PI_STONE;
        return (terrain_h >= SEA_LEVEL) ? PI_GRASS : PI_DIRT;
    }
    if (beach && wy > terrain_h - 4) return PI_SAND;
    if (wy > terrain_h - 4) return PI_DIRT;
    return PI_STONE;
}

static void write_air_section(PacketBuf& buf) {
    pkt_write_i16(buf, 0);
    pkt_write_byte(buf, 0);
    pkt_write_varint(buf, S_AIR);
    pkt_write_varint(buf, 0);
    pkt_write_byte(buf, 0);
    pkt_write_varint(buf, 0);
    pkt_write_varint(buf, 0);
}

static void write_section(PacketBuf& buf, int cx, int cz, int si,
                           int heights[16][16], TreeInfo* trees, int tcnt) {
    int base_y = si * 16 + MIN_Y;

    int max_h = -999;
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int h = heights[x][z];
            if (h < SEA_LEVEL && SEA_LEVEL > max_h) max_h = SEA_LEVEL;
            if (h > max_h) max_h = h;
        }
    for (int t = 0; t < tcnt; t++) {
        int top = trees[t].ground + 5;
        if (top > max_h) max_h = top;
    }

    if (base_y > max_h + 1) { write_air_section(buf); return; }

    int block_count = 0;
    int64_t longs[256];
    memset(longs, 0, sizeof(longs));

    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                int wx = cx * 16 + x, wy = base_y + y, wz = cz * 16 + z;
                int pi = get_block(wx, wy, wz, heights[x][z], trees, tcnt);
                if (pi != PI_AIR) block_count++;
                int idx = x + z * 16 + y * 256;
                longs[idx / 16] |= (static_cast<int64_t>(pi) & 0xF) << ((idx % 16) * 4);
            }

    if (block_count == 0) { write_air_section(buf); return; }

    pkt_write_i16(buf, static_cast<int16_t>(block_count));
    pkt_write_byte(buf, 4);
    pkt_write_varint(buf, PALETTE_SIZE);
    for (int i = 0; i < PALETTE_SIZE; i++) pkt_write_varint(buf, PALETTE[i]);
    pkt_write_varint(buf, 256);
    for (int i = 0; i < 256; i++) pkt_write_i64(buf, longs[i]);

    pkt_write_byte(buf, 0);
    pkt_write_varint(buf, 0);
    pkt_write_varint(buf, 0);
}

static void compute_sky_light(uint8_t* light, int cx, int cz, int si,
                               int sky_h[16][16]) {
    int base_y = si * 16 + MIN_Y;
    memset(light, 0, 2048);
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                int wy = base_y + y;
                int sky = (wy > sky_h[x][z]) ? 15 : 0;
                int idx = x + z * 16 + y * 256;
                if (idx & 1) light[idx / 2] |= (sky << 4);
                else         light[idx / 2] |= sky;
            }
}

void send_chunk(int sock, PacketBuf& out, PacketBuf& scratch, int cx, int cz) {
    int heights[16][16];
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++)
            heights[x][z] = terrain_height(cx * 16 + x, cz * 16 + z);

    TreeInfo trees[32];
    int tcnt = find_trees(cx, cz, trees, 32);

    int sky_h[16][16];
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int h = heights[x][z];
            if (h < SEA_LEVEL) h = SEA_LEVEL;
            for (int t = 0; t < tcnt; t++) {
                int dx = (cx * 16 + x) - trees[t].bx;
                int dz = (cz * 16 + z) - trees[t].bz;
                int mdy = max_tree_dy(dx, dz);
                if (mdy >= 0) {
                    int ty = trees[t].ground + 1 + mdy;
                    if (ty > h) h = ty;
                }
            }
            sky_h[x][z] = h;
        }

    scratch.reset();
    for (int s = 0; s < NUM_SECTIONS; s++)
        write_section(scratch, cx, cz, s, heights, trees, tcnt);

    int64_t hm_longs[37];
    memset(hm_longs, 0, sizeof(hm_longs));
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            int col = x + z * 16;
            int val = sky_h[x][z] - MIN_Y + 1;
            if (val < 0) val = 0;
            hm_longs[col / 7] |= (static_cast<int64_t>(val) & 0x1FF) << ((col % 7) * 9);
        }

    out.reset();
    pkt_write_varint(out, 0x28);
    pkt_write_i32(out, cx);
    pkt_write_i32(out, cz);
    nbt_begin(out);
    nbt_long_array(out, "MOTION_BLOCKING", hm_longs, 37);
    nbt_end(out);
    pkt_write_varint(out, static_cast<int32_t>(scratch.len));
    out.append(scratch.data, scratch.len);
    pkt_write_varint(out, 0);

    // Sky light: sections 0, 1, 2 (bits 1, 2, 3)
    pkt_write_varint(out, 1);
    pkt_write_i64(out, 0x0ELL);
    // Block light: none
    pkt_write_varint(out, 0);
    // Empty sky light: section -1 (bit 0)
    pkt_write_varint(out, 1);
    pkt_write_i64(out, 0x01LL);
    // Empty block light: all 26 sections
    pkt_write_varint(out, 1);
    pkt_write_i64(out, 0x03FFFFFFLL);

    // Sky light arrays (3 sections)
    pkt_write_varint(out, 3);
    uint8_t light[2048];
    for (int s = 0; s < 2; s++) {
        compute_sky_light(light, cx, cz, s, sky_h);
        pkt_write_varint(out, 2048);
        out.append(light, 2048);
    }
    // Section 2: all sky light 15
    memset(light, 0xFF, 2048);
    pkt_write_varint(out, 2048);
    out.append(light, 2048);

    // Block light arrays: none
    pkt_write_varint(out, 0);

    out.send_packet(sock);
}

void send_center_chunk(int sock, PacketBuf& out, int cx, int cz) {
    out.reset();
    pkt_write_varint(out, 0x58);
    pkt_write_varint(out, cx);
    pkt_write_varint(out, cz);
    out.send_packet(sock);
}

static void send_login(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x2C);
    pkt_write_i32(out, 1);
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
    pkt_write_i64(out, 0);
    pkt_write_byte(out, 1);
    pkt_write_byte(out, 0xFF);
    pkt_write_bool(out, false);
    pkt_write_bool(out, true);
    pkt_write_bool(out, false);
    pkt_write_varint(out, 0);
    pkt_write_varint(out, 63);
    pkt_write_bool(out, false);
    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent Login (Play)");
}

static void send_game_event(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x23);
    pkt_write_byte(out, 13);
    pkt_write_f32(out, 0.0f);
    out.send_packet(sock);
}

void send_play_packets(int sock, PacketBuf& out) {
    send_login(sock, out);
    send_game_event(sock, out);
    send_center_chunk(sock, out, 0, 0);

    PacketBuf scratch;
    scratch.init(8192);
    int vd = MC_VIEW_DISTANCE;
    for (int cx = -vd; cx <= vd; cx++)
        for (int cz = -vd; cz <= vd; cz++)
            send_chunk(sock, out, scratch, cx, cz);
    scratch.free();
    ESP_LOGI(TAG, "Sent %d chunks", (2 * vd + 1) * (2 * vd + 1));

    int spawn_y = terrain_height(0, 0) + 1;

    out.reset();
    pkt_write_varint(out, 0x5B);
    pkt_write_position(out, 0, spawn_y, 0);
    pkt_write_f32(out, 0.0f);
    out.send_packet(sock);

    out.reset();
    pkt_write_varint(out, 0x42);
    pkt_write_varint(out, 1);
    pkt_write_f64(out, 0.5);
    pkt_write_f64(out, static_cast<double>(spawn_y));
    pkt_write_f64(out, 0.5);
    pkt_write_f64(out, 0.0);
    pkt_write_f64(out, 0.0);
    pkt_write_f64(out, 0.0);
    pkt_write_f32(out, 0.0f);
    pkt_write_f32(out, 0.0f);
    pkt_write_i32(out, 0);
    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent spawn at Y=%d", spawn_y);
}
