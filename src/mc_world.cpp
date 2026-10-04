#include "mc_world.h"
#include "mc_types.h"
#include "mc_nbt.h"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstring>

const BiomeInfo BIOMES[BIOME_COUNT] = {
    {"minecraft:plains",          0.8f,  0.4f, 7907327},
    {"minecraft:taiga",           0.25f, 0.8f, 8233983},
    {"minecraft:ocean",           0.5f,  0.5f, 8103167},
    {"minecraft:windswept_hills", 0.2f,  0.3f, 8233727},
};

enum : uint8_t {
    B_AIR, B_STONE, B_DIRT, B_GRASS, B_WATER, B_OAK_LOG, B_OAK_LEAVES, B_SHORT_GRASS,
    B_SAND, B_SPRUCE_LOG, B_SPRUCE_LEAVES, B_FERN, B_PODZOL, B_GRAVEL, B_SNOW,
    B_PALETTE_SIZE
};

// 1.21.4 block state ids. Grass/podzol are snowy=false, leaves are
// distance=1,persistent=true,waterlogged=false, logs are axis=y.
static const int32_t PALETTE[B_PALETTE_SIZE] = {
    0, 1, 10, 9, 86, 137, 253, 2048, 118, 140, 281, 2049, 13, 124, 5950,
};

enum : uint32_t {
    SALT_CONT = 0x1001, SALT_TEMP = 0x2002, SALT_MTN = 0x3003, SALT_DETAIL = 0x4004,
    SALT_PEAK = 0x5005, SALT_DECOR = 0x6006, SALT_TREE = 0x7007,
};

static uint32_t g_seed;
static int g_spawn_x, g_spawn_y, g_spawn_z;

static uint32_t hash2(int x, int z, uint32_t salt) {
    uint32_t h = g_seed ^ salt;
    h ^= static_cast<uint32_t>(x) * 0x27d4eb2dU;
    h = (h ^ (h >> 15)) * 0x85ebca6bU;
    h ^= static_cast<uint32_t>(z) * 0x165667b1U;
    h = (h ^ (h >> 13)) * 0xc2b2ae35U;
    return h ^ (h >> 16);
}

static float grad(uint32_t h, float x, float z) {
    switch (h & 7) {
        case 0:  return  x + z;
        case 1:  return  x - z;
        case 2:  return -x + z;
        case 3:  return -x - z;
        case 4:  return  x;
        case 5:  return -x;
        case 6:  return  z;
        default: return -z;
    }
}

static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }

static float noise2(uint32_t salt, float x, float z) {
    float fx = floorf(x), fz = floorf(z);
    int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
    float tx = x - fx, tz = z - fz;
    float n00 = grad(hash2(ix,     iz,     salt), tx,        tz);
    float n10 = grad(hash2(ix + 1, iz,     salt), tx - 1.0f, tz);
    float n01 = grad(hash2(ix,     iz + 1, salt), tx,        tz - 1.0f);
    float n11 = grad(hash2(ix + 1, iz + 1, salt), tx - 1.0f, tz - 1.0f);
    float u = fade(tx), v = fade(tz);
    return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
}

static float fbm(uint32_t salt, float x, float z, int octaves) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += noise2(salt + i * 0x9E3779B9U, x, z) * amp;
        norm += amp;
        amp *= 0.5f;
        x *= 2.0f;
        z *= 2.0f;
    }
    return sum / norm;
}

static float smoothstep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

struct Column {
    int16_t h;
    Biome biome;
    uint8_t top, under, plant;
};

static void column_at(int wx, int wz, Column& c) {
    float x = static_cast<float>(wx), z = static_cast<float>(wz);
    float cont   = fbm(SALT_CONT,   x / 384.0f, z / 384.0f, 4);
    float temp   = fbm(SALT_TEMP,   x / 512.0f, z / 512.0f, 3);
    float mtn    = fbm(SALT_MTN,    x / 256.0f, z / 256.0f, 3);
    float detail = fbm(SALT_DETAIL, x / 48.0f,  z / 48.0f,  3);

    float land = smoothstep(-0.30f, -0.05f, cont);
    float hills = smoothstep(0.08f, 0.38f, mtn) * land;
    float hf = SEA_LEVEL - 9 + land * 13.0f + detail * (2.0f + 4.0f * land);
    if (hills > 0.0f)
        hf += hills * (16.0f + 10.0f * fbm(SALT_PEAK, x / 64.0f, z / 64.0f, 2));

    int h = static_cast<int>(floorf(hf));
    if (h < MIN_Y + 1) h = MIN_Y + 1;
    if (h > MIN_Y + NUM_SECTIONS * 16 - 16) h = MIN_Y + NUM_SECTIONS * 16 - 16;
    c.h = static_cast<int16_t>(h);

    if (h < SEA_LEVEL && land < 0.5f) c.biome = BIOME_OCEAN;
    else if (hills > 0.5f)            c.biome = BIOME_HILLS;
    else if (temp < -0.1f)            c.biome = BIOME_TAIGA;
    else                              c.biome = BIOME_PLAINS;

    uint32_t r = hash2(wx, wz, SALT_DECOR);
    c.plant = B_AIR;
    c.under = B_DIRT;
    if (h < SEA_LEVEL) {
        c.top = c.under = (h < SEA_LEVEL - 5) ? B_GRAVEL : B_SAND;
    } else if (h <= SEA_LEVEL + 1 && land < 0.9f) {
        c.top = c.under = B_SAND;
    } else if (c.biome == BIOME_HILLS && h > SEA_LEVEL + 30) {
        c.top = B_SNOW;
        c.under = B_STONE;
    } else if (c.biome == BIOME_HILLS && h > SEA_LEVEL + 16) {
        c.top = ((r & 15) == 0) ? B_GRAVEL : B_STONE;
        c.under = B_STONE;
    } else if (c.biome == BIOME_TAIGA) {
        c.top = (((r >> 8) & 3) == 0) ? B_PODZOL : B_GRASS;
        if ((r & 7) < 2) c.plant = B_FERN;
        else if ((r & 7) == 2) c.plant = B_SHORT_GRASS;
    } else {
        c.top = B_GRASS;
        if ((r & 7) < (c.biome == BIOME_PLAINS ? 3u : 1u)) c.plant = B_SHORT_GRASS;
    }
}

static int tree_height(int wx, int wz, const Column& c) {
    uint32_t r = hash2(wx, wz, SALT_TREE);
    if (r % 14 != 0) return 0;
    if (c.h < SEA_LEVEL + 1 || (c.top != B_GRASS && c.top != B_PODZOL)) return 0;
    uint32_t r2 = r / 14;
    if (c.biome == BIOME_TAIGA) return 6 + static_cast<int>((r2 >> 4) % 3);
    if (c.biome == BIOME_PLAINS && r2 % 6 == 0) return 4 + static_cast<int>((r2 >> 4) & 1);
    if (c.biome == BIOME_HILLS && r2 % 10 == 0) return 4 + static_cast<int>((r2 >> 4) & 1);
    return 0;
}

static bool is_plant(uint8_t b) { return b == B_SHORT_GRASS || b == B_FERN; }

static void put(ChunkScratch& w, int lx, int y, int lz, uint8_t b, bool force) {
    if (lx < 0 || lx > 15 || lz < 0 || lz > 15) return;
    int yi = y - MIN_Y;
    if (yi < 0 || yi >= NUM_SECTIONS * 16) return;
    uint8_t& cur = w.blocks[lx + lz * 16 + yi * 256];
    if (force || cur == B_AIR || is_plant(cur)) cur = b;
}

static void leaf_layer(ChunkScratch& w, int lx, int y, int lz, int r, uint8_t leaf) {
    for (int dx = -r; dx <= r; dx++)
        for (int dz = -r; dz <= r; dz++) {
            if (r == 2 && abs(dx) == 2 && abs(dz) == 2) continue;
            if (r == 1 && abs(dx) == 1 && abs(dz) == 1 && (y & 1)) continue;
            put(w, lx + dx, y, lz + dz, leaf, false);
        }
}

static void stamp_tree(ChunkScratch& w, int lx, int lz, int ground, int height, Biome biome) {
    int base = ground + 1;
    if (biome == BIOME_TAIGA) {
        for (int dy = 2; dy <= height; dy++) {
            int k = height - dy;
            int r = (k == 0) ? 0 : (k % 2 == 1 ? 1 : 2);
            leaf_layer(w, lx, base + dy, lz, r, B_SPRUCE_LEAVES);
        }
        put(w, lx, base + height + 1, lz, B_SPRUCE_LEAVES, false);
        for (int dy = 0; dy < height; dy++) put(w, lx, base + dy, lz, B_SPRUCE_LOG, true);
    } else {
        leaf_layer(w, lx, base + height - 2, lz, 2, B_OAK_LEAVES);
        leaf_layer(w, lx, base + height - 1, lz, 2, B_OAK_LEAVES);
        leaf_layer(w, lx, base + height, lz, 1, B_OAK_LEAVES);
        put(w, lx, base + height + 1, lz, B_OAK_LEAVES, false);
        for (int dy = 0; dy < height; dy++) put(w, lx, base + dy, lz, B_OAK_LOG, true);
    }
}

void world_init(uint32_t seed) {
    g_seed = seed;
    g_spawn_x = 8;
    g_spawn_z = 8;
    g_spawn_y = SEA_LEVEL + 1;

    for (int ring = 0; ring <= 64; ring++)
        for (int dx = -ring; dx <= ring; dx++)
            for (int dz = -ring; dz <= ring; dz++) {
                if (abs(dx) != ring && abs(dz) != ring) continue;
                int x = dx * 16 + 8, z = dz * 16 + 8;
                Column c;
                column_at(x, z, c);
                if (c.h >= SEA_LEVEL + 1 && c.top == B_GRASS && tree_height(x, z, c) == 0) {
                    g_spawn_x = x;
                    g_spawn_z = z;
                    g_spawn_y = c.h + 1;
                    return;
                }
            }
}

uint32_t world_seed() { return g_seed; }

void world_spawn(int& x, int& y, int& z) {
    x = g_spawn_x;
    y = g_spawn_y;
    z = g_spawn_z;
}

Biome world_biome_at(int x, int z) {
    Column c;
    column_at(x, z, c);
    return c.biome;
}

int world_height_at(int x, int z) {
    Column c;
    column_at(x, z, c);
    return c.h;
}

bool ChunkScratch::init() {
    blocks = static_cast<uint8_t*>(heap_caps_malloc(NUM_SECTIONS * 4096, MALLOC_CAP_SPIRAM));
    light = static_cast<uint8_t*>(heap_caps_malloc(NUM_SECTIONS * 2048, MALLOC_CAP_SPIRAM));
    return blocks && light && sections.init(16384);
}

void ChunkScratch::free() {
    if (blocks) { heap_caps_free(blocks); blocks = nullptr; }
    if (light) { heap_caps_free(light); light = nullptr; }
    sections.free();
}

static bool is_opaque(uint8_t b) {
    return b != B_AIR && b != B_WATER && b != B_OAK_LEAVES && b != B_SPRUCE_LEAVES && !is_plant(b);
}

static size_t encode_biomes(uint8_t* buf, const Column cols[16][16]) {
    int ids[BIOME_COUNT];
    int n = 0;
    uint8_t cell_idx[16];
    for (int qz = 0; qz < 4; qz++)
        for (int qx = 0; qx < 4; qx++) {
            int b = cols[qx * 4 + 2][qz * 4 + 2].biome;
            int i = 0;
            while (i < n && ids[i] != b) i++;
            if (i == n) ids[n++] = b;
            cell_idx[qx + qz * 4] = static_cast<uint8_t>(i);
        }

    size_t p = 0;
    if (n == 1) {
        buf[p++] = 0;
        buf[p++] = static_cast<uint8_t>(ids[0]);
        buf[p++] = 0;
        return p;
    }

    int bpe = (n <= 2) ? 1 : 2;
    int per_long = 64 / bpe;
    int nlongs = (64 + per_long - 1) / per_long;
    int64_t longs[2] = {0, 0};
    for (int i = 0; i < 64; i++) {
        int cell = (i % 4) + ((i / 4) % 4) * 4;
        longs[i / per_long] |= static_cast<int64_t>(cell_idx[cell]) << ((i % per_long) * bpe);
    }
    buf[p++] = static_cast<uint8_t>(bpe);
    buf[p++] = static_cast<uint8_t>(n);
    for (int i = 0; i < n; i++) buf[p++] = static_cast<uint8_t>(ids[i]);
    buf[p++] = static_cast<uint8_t>(nlongs);
    for (int i = 0; i < nlongs; i++) {
        mc_write_i64(buf + p, longs[i]);
        p += 8;
    }
    return p;
}

void world_encode_chunk(PacketBuf& out, ChunkScratch& w, int cx, int cz) {
    Column cols[16][16];
    int top_y = SEA_LEVEL;
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            column_at(cx * 16 + x, cz * 16 + z, cols[x][z]);
            if (cols[x][z].h > top_y) top_y = cols[x][z].h;
        }
    top_y += 11;
    int top_sec = (top_y - MIN_Y) / 16;
    if (top_sec > NUM_SECTIONS - 1) top_sec = NUM_SECTIONS - 1;
    int ny = (top_sec + 1) * 16;

    memset(w.blocks, B_AIR, ny * 256);
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            const Column& c = cols[x][z];
            for (int y = MIN_Y; y <= c.h; y++) {
                int depth = c.h - y;
                uint8_t b = (depth == 0) ? c.top : (depth <= 3 ? c.under : static_cast<uint8_t>(B_STONE));
                w.blocks[x + z * 16 + (y - MIN_Y) * 256] = b;
            }
            for (int y = c.h + 1; y <= SEA_LEVEL; y++)
                w.blocks[x + z * 16 + (y - MIN_Y) * 256] = B_WATER;
            if (c.h >= SEA_LEVEL && c.plant != B_AIR)
                w.blocks[x + z * 16 + (c.h + 1 - MIN_Y) * 256] = c.plant;
        }

    // Trees from neighbouring chunks can hang over the edge, so scan a 2-block border
    for (int wx = cx * 16 - 2; wx < cx * 16 + 18; wx++)
        for (int wz = cz * 16 - 2; wz < cz * 16 + 18; wz++) {
            if (hash2(wx, wz, SALT_TREE) % 14 != 0) continue;
            int lx = wx - cx * 16, lz = wz - cz * 16;
            Column c;
            if (lx >= 0 && lx < 16 && lz >= 0 && lz < 16) c = cols[lx][lz];
            else column_at(wx, wz, c);
            int th = tree_height(wx, wz, c);
            if (th > 0) stamp_tree(w, lx, lz, c.h, th, c.biome);
        }

    int64_t hm_longs[37];
    memset(hm_longs, 0, sizeof(hm_longs));
    memset(w.light, 0, (top_sec + 1) * 2048);
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            int light = 15, hm = 0;
            for (int yi = ny - 1; yi >= 0; yi--) {
                uint8_t b = w.blocks[x + z * 16 + yi * 256];
                if (hm == 0 && b != B_AIR && !is_plant(b)) hm = yi + 1;
                if (is_opaque(b)) light = 0;
                else if (b != B_AIR && !is_plant(b) && light > 0) light--;
                int idx = x + z * 16 + (yi & 15) * 256;
                w.light[(yi >> 4) * 2048 + idx / 2] |= static_cast<uint8_t>(light << ((idx & 1) * 4));
            }
            int col = x + z * 16;
            hm_longs[col / 7] |= static_cast<int64_t>(hm & 0x1FF) << ((col % 7) * 9);
        }

    uint8_t biome_bytes[32];
    size_t biome_len = encode_biomes(biome_bytes, cols);

    PacketBuf& sec = w.sections;
    sec.reset();
    for (int s = 0; s < NUM_SECTIONS; s++) {
        int count = 0;
        const uint8_t* blk = w.blocks + s * 4096;
        if (s <= top_sec)
            for (int i = 0; i < 4096; i++) count += (blk[i] != B_AIR);

        if (count == 0) {
            pkt_write_i16(sec, 0);
            pkt_write_byte(sec, 0);
            pkt_write_varint(sec, PALETTE[B_AIR]);
            pkt_write_varint(sec, 0);
        } else {
            pkt_write_i16(sec, static_cast<int16_t>(count));
            pkt_write_byte(sec, 4);
            pkt_write_varint(sec, B_PALETTE_SIZE);
            for (int i = 0; i < B_PALETTE_SIZE; i++) pkt_write_varint(sec, PALETTE[i]);
            pkt_write_varint(sec, 256);
            for (int l = 0; l < 256; l++) {
                int64_t v = 0;
                for (int k = 0; k < 16; k++)
                    v |= static_cast<int64_t>(blk[l * 16 + k] & 0xF) << (k * 4);
                pkt_write_i64(sec, v);
            }
        }
        sec.append(biome_bytes, biome_len);
    }

    out.reset();
    pkt_write_varint(out, 0x28);
    pkt_write_i32(out, cx);
    pkt_write_i32(out, cz);
    nbt_begin(out);
    nbt_long_array(out, "MOTION_BLOCKING", hm_longs, 37);
    nbt_end(out);
    pkt_write_varint(out, static_cast<int32_t>(sec.len));
    out.append(sec.data, sec.len);
    pkt_write_varint(out, 0);

    // Light sections are offset by one from chunk sections (index 0 is below the world).
    // Sections above top_sec get no data, which the client treats as open sky.
    pkt_write_varint(out, 1);
    pkt_write_i64(out, ((1LL << (top_sec + 1)) - 1) << 1);
    pkt_write_varint(out, 0);
    pkt_write_varint(out, 1);
    pkt_write_i64(out, 0x01LL);
    pkt_write_varint(out, 1);
    pkt_write_i64(out, 0x03FFFFFFLL);

    pkt_write_varint(out, top_sec + 1);
    for (int s = 0; s <= top_sec; s++) {
        pkt_write_varint(out, 2048);
        out.append(w.light + s * 2048, 2048);
    }
    pkt_write_varint(out, 0);
}
