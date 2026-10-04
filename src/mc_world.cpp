#include "mc_world.h"
#include "mc_types.h"
#include "mc_nbt.h"
#include "mc_protocol.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

// 1.21.4 block state ids, indexed by Block. Grass/podzol are snowy=false, leaves are
// distance=1,persistent=true,waterlogged=false, logs are axis=y.
static constexpr auto PALETTE = std::to_array<int32_t>({
    0, 1, 10, 9, 86, 137, 253, 2048, 118, 140, 281, 2049, 13, 124, 5950,
});
static_assert(PALETTE.size() == std::to_underlying(Block::Snow) + 1);
static_assert(PALETTE.size() <= 16, "sections are sent with 4 bits per block");

enum : uint32_t {
    SALT_CONT = 0x1001, SALT_TEMP = 0x2002, SALT_MTN = 0x3003, SALT_DETAIL = 0x4004,
    SALT_PEAK = 0x5005, SALT_DECOR = 0x6006, SALT_TREE = 0x7007,
};

static uint32_t g_seed;
static BlockPos g_spawn{8, SEA_LEVEL + 1, 8};

static uint32_t hash2(int x, int z, uint32_t salt) {
    uint32_t h = g_seed ^ salt;
    h ^= static_cast<uint32_t>(x) * 0x27d4eb2dU;
    h = (h ^ (h >> 15)) * 0x85ebca6bU;
    h ^= static_cast<uint32_t>(z) * 0x165667b1U;
    h = (h ^ (h >> 13)) * 0xc2b2ae35U;
    return h ^ (h >> 16);
}

static constexpr float grad(uint32_t h, float x, float z) {
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

static constexpr float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }

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

static constexpr float smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Column {
    int16_t h;
    Biome biome;
    Block top, under, plant;
};

using ChunkColumns = std::array<std::array<Column, 16>, 16>;

static Column column_at(int wx, int wz) {
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

    int h = std::clamp(static_cast<int>(floorf(hf)), MIN_Y + 1, MIN_Y + NUM_SECTIONS * 16 - 16);
    Column c{.h = static_cast<int16_t>(h), .biome = Biome::Plains,
             .top = Block::Grass, .under = Block::Dirt, .plant = Block::Air};

    if (h < SEA_LEVEL && land < 0.5f) c.biome = Biome::Ocean;
    else if (hills > 0.5f)            c.biome = Biome::Hills;
    else if (temp < -0.1f)            c.biome = Biome::Taiga;

    uint32_t r = hash2(wx, wz, SALT_DECOR);
    if (h < SEA_LEVEL) {
        c.top = c.under = (h < SEA_LEVEL - 5) ? Block::Gravel : Block::Sand;
    } else if (h <= SEA_LEVEL + 1 && land < 0.9f) {
        c.top = c.under = Block::Sand;
    } else if (c.biome == Biome::Hills && h > SEA_LEVEL + 30) {
        c.top = Block::Snow;
        c.under = Block::Stone;
    } else if (c.biome == Biome::Hills && h > SEA_LEVEL + 16) {
        c.top = ((r & 15) == 0) ? Block::Gravel : Block::Stone;
        c.under = Block::Stone;
    } else if (c.biome == Biome::Taiga) {
        c.top = (((r >> 8) & 3) == 0) ? Block::Podzol : Block::Grass;
        if ((r & 7) < 2) c.plant = Block::Fern;
        else if ((r & 7) == 2) c.plant = Block::ShortGrass;
    } else if ((r & 7) < (c.biome == Biome::Plains ? 3u : 1u)) {
        c.plant = Block::ShortGrass;
    }
    return c;
}

static int tree_height(int wx, int wz, const Column& c) {
    uint32_t r = hash2(wx, wz, SALT_TREE);
    if (r % 14 != 0) return 0;
    if (c.h < SEA_LEVEL + 1 || (c.top != Block::Grass && c.top != Block::Podzol)) return 0;
    uint32_t r2 = r / 14;
    switch (c.biome) {
        case Biome::Taiga:  return 6 + static_cast<int>((r2 >> 4) % 3);
        case Biome::Plains: return r2 % 6 == 0 ? 4 + static_cast<int>((r2 >> 4) & 1) : 0;
        case Biome::Hills:  return r2 % 10 == 0 ? 4 + static_cast<int>((r2 >> 4) & 1) : 0;
        default:            return 0;
    }
}

static constexpr bool is_plant(Block b) { return b == Block::ShortGrass || b == Block::Fern; }

static constexpr bool is_opaque(Block b) {
    return b != Block::Air && b != Block::Water && b != Block::OakLeaves && b != Block::SpruceLeaves && !is_plant(b);
}

static void put(ChunkScratch& w, int lx, int y, int lz, Block b, bool force) {
    if (lx < 0 || lx > 15 || lz < 0 || lz > 15) return;
    int yi = y - MIN_Y;
    if (yi < 0 || yi >= NUM_SECTIONS * 16) return;
    Block& cur = w.blocks[lx + lz * 16 + yi * 256];
    if (force || cur == Block::Air || is_plant(cur)) cur = b;
}

static void leaf_layer(ChunkScratch& w, int lx, int y, int lz, int r, Block leaf) {
    for (int dx = -r; dx <= r; dx++)
        for (int dz = -r; dz <= r; dz++) {
            if (r == 2 && std::abs(dx) == 2 && std::abs(dz) == 2) continue;
            if (r == 1 && std::abs(dx) == 1 && std::abs(dz) == 1 && (y & 1)) continue;
            put(w, lx + dx, y, lz + dz, leaf, false);
        }
}

static void stamp_tree(ChunkScratch& w, int lx, int lz, int ground, int height, Biome biome) {
    int base = ground + 1;
    if (biome == Biome::Taiga) {
        for (int dy = 2; dy <= height; dy++) {
            int k = height - dy;
            int r = (k == 0) ? 0 : (k % 2 == 1 ? 1 : 2);
            leaf_layer(w, lx, base + dy, lz, r, Block::SpruceLeaves);
        }
        put(w, lx, base + height + 1, lz, Block::SpruceLeaves, false);
        for (int dy = 0; dy < height; dy++) put(w, lx, base + dy, lz, Block::SpruceLog, true);
    } else {
        leaf_layer(w, lx, base + height - 2, lz, 2, Block::OakLeaves);
        leaf_layer(w, lx, base + height - 1, lz, 2, Block::OakLeaves);
        leaf_layer(w, lx, base + height, lz, 1, Block::OakLeaves);
        put(w, lx, base + height + 1, lz, Block::OakLeaves, false);
        for (int dy = 0; dy < height; dy++) put(w, lx, base + dy, lz, Block::OakLog, true);
    }
}

void world_init(uint32_t seed) {
    g_seed = seed;
    g_spawn = {8, SEA_LEVEL + 1, 8};

    for (int ring = 0; ring <= 64; ring++)
        for (int dx = -ring; dx <= ring; dx++)
            for (int dz = -ring; dz <= ring; dz++) {
                if (std::abs(dx) != ring && std::abs(dz) != ring) continue;
                int x = dx * 16 + 8, z = dz * 16 + 8;
                Column c = column_at(x, z);
                if (c.h >= SEA_LEVEL + 1 && c.top == Block::Grass && tree_height(x, z, c) == 0) {
                    g_spawn = {x, c.h + 1, z};
                    return;
                }
            }
}

uint32_t world_seed() { return g_seed; }
BlockPos world_spawn() { return g_spawn; }
Biome world_biome_at(int x, int z) { return column_at(x, z).biome; }
int world_height_at(int x, int z) { return column_at(x, z).h; }

struct BiomeBytes {
    std::array<uint8_t, 32> buf{};
    size_t size = 0;
    std::span<const uint8_t> bytes() const { return {buf.data(), size}; }
};

static BiomeBytes encode_biomes(const ChunkColumns& cols) {
    std::array<uint8_t, BIOMES.size()> ids{};
    size_t n = 0;
    std::array<uint8_t, 16> cell_idx{};
    for (int qz = 0; qz < 4; qz++)
        for (int qx = 0; qx < 4; qx++) {
            auto b = std::to_underlying(cols[qx * 4 + 2][qz * 4 + 2].biome);
            auto it = std::ranges::find(ids.begin(), ids.begin() + n, b);
            if (it == ids.begin() + n) ids[n++] = b;
            cell_idx[qx + qz * 4] = static_cast<uint8_t>(it - ids.begin());
        }

    BiomeBytes out;
    auto push = [&](uint8_t v) { out.buf[out.size++] = v; };
    if (n == 1) {
        push(0);
        push(ids[0]);
        push(0);
        return out;
    }

    int bpe = (n <= 2) ? 1 : 2;
    int per_long = 64 / bpe;
    int nlongs = (64 + per_long - 1) / per_long;
    std::array<int64_t, 2> longs{};
    for (int i = 0; i < 64; i++) {
        int cell = (i % 4) + ((i / 4) % 4) * 4;
        longs[i / per_long] |= static_cast<int64_t>(cell_idx[cell]) << ((i % per_long) * bpe);
    }
    push(static_cast<uint8_t>(bpe));
    push(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; i++) push(ids[i]);
    push(static_cast<uint8_t>(nlongs));
    for (int i = 0; i < nlongs; i++)
        for (uint8_t byte : to_be(longs[i])) push(byte);
    return out;
}

void world_encode_chunk(PacketBuf& out, ChunkScratch& w, int cx, int cz) {
    ChunkColumns cols;
    int top_y = SEA_LEVEL;
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            cols[x][z] = column_at(cx * 16 + x, cz * 16 + z);
            top_y = std::max<int>(top_y, cols[x][z].h);
        }
    int top_sec = std::min((top_y + 11 - MIN_Y) / 16, NUM_SECTIONS - 1);
    int ny = (top_sec + 1) * 16;

    Block* blocks = w.blocks.get();
    std::fill_n(blocks, ny * 256, Block::Air);
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            const Column& c = cols[x][z];
            for (int y = MIN_Y; y <= c.h; y++) {
                int depth = c.h - y;
                blocks[x + z * 16 + (y - MIN_Y) * 256] = (depth == 0) ? c.top : (depth <= 3 ? c.under : Block::Stone);
            }
            for (int y = c.h + 1; y <= SEA_LEVEL; y++)
                blocks[x + z * 16 + (y - MIN_Y) * 256] = Block::Water;
            if (c.h >= SEA_LEVEL && c.plant != Block::Air)
                blocks[x + z * 16 + (c.h + 1 - MIN_Y) * 256] = c.plant;
        }

    // Trees from neighbouring chunks can hang over the edge, so scan a 2-block border
    for (int wx = cx * 16 - 2; wx < cx * 16 + 18; wx++)
        for (int wz = cz * 16 - 2; wz < cz * 16 + 18; wz++) {
            if (hash2(wx, wz, SALT_TREE) % 14 != 0) continue;
            int lx = wx - cx * 16, lz = wz - cz * 16;
            bool inside = lx >= 0 && lx < 16 && lz >= 0 && lz < 16;
            Column c = inside ? cols[lx][lz] : column_at(wx, wz);
            if (int th = tree_height(wx, wz, c); th > 0) stamp_tree(w, lx, lz, c.h, th, c.biome);
        }

    std::array<int64_t, 37> hm_longs{};
    uint8_t* light = w.light.get();
    std::fill_n(light, (top_sec + 1) * 2048, 0);
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++) {
            int sky = 15, hm = 0;
            for (int yi = ny - 1; yi >= 0; yi--) {
                Block b = blocks[x + z * 16 + yi * 256];
                if (hm == 0 && b != Block::Air && !is_plant(b)) hm = yi + 1;
                if (is_opaque(b)) sky = 0;
                else if (b != Block::Air && !is_plant(b) && sky > 0) sky--;
                int idx = x + z * 16 + (yi & 15) * 256;
                light[(yi >> 4) * 2048 + idx / 2] |= static_cast<uint8_t>(sky << ((idx & 1) * 4));
            }
            int col = x + z * 16;
            hm_longs[col / 7] |= static_cast<int64_t>(hm & 0x1FF) << ((col % 7) * 9);
        }

    BiomeBytes biomes = encode_biomes(cols);

    PacketBuf& sec = w.sections;
    sec.reset();
    for (int s = 0; s < NUM_SECTIONS; s++) {
        std::span<const Block> blk(blocks + s * 4096, 4096);
        auto count = s <= top_sec ? std::ranges::count_if(blk, [](Block b) { return b != Block::Air; }) : 0;

        if (count == 0) {
            pkt_write_i16(sec, 0);
            pkt_write_byte(sec, 0);
            pkt_write_varint(sec, PALETTE[std::to_underlying(Block::Air)]);
            pkt_write_varint(sec, 0);
        } else {
            pkt_write_i16(sec, static_cast<int16_t>(count));
            pkt_write_byte(sec, 4);
            pkt_write_varint(sec, static_cast<int32_t>(PALETTE.size()));
            for (int32_t state : PALETTE) pkt_write_varint(sec, state);
            pkt_write_varint(sec, 256);
            for (int l = 0; l < 256; l++) {
                int64_t v = 0;
                for (int k = 0; k < 16; k++)
                    v |= static_cast<int64_t>(std::to_underlying(blk[l * 16 + k]) & 0xF) << (k * 4);
                pkt_write_i64(sec, v);
            }
        }
        sec.append(biomes.bytes());
    }

    pkt_begin(out, PlayOut::ChunkData);
    pkt_write_i32(out, cx);
    pkt_write_i32(out, cz);
    nbt_begin(out);
    nbt_long_array(out, "MOTION_BLOCKING", hm_longs);
    nbt_end(out);
    pkt_write_varint(out, static_cast<int32_t>(sec.len));
    out.append(sec.bytes());
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
        out.append({light + s * 2048, 2048});
    }
    pkt_write_varint(out, 0);
}
