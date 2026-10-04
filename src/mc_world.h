#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include "mc_packet.h"

inline constexpr int SEA_LEVEL = -52;
inline constexpr int MIN_Y = -64;
inline constexpr int NUM_SECTIONS = 24;

enum class Biome : uint8_t { Plains, Taiga, Ocean, Hills };

struct BiomeInfo {
    std::string_view id;
    float temperature;
    float downfall;
    int32_t sky_color;
};

// The index is the registry id the client sees, so this follows the Biome order
inline constexpr std::array<BiomeInfo, 4> BIOMES{{
    {"minecraft:plains",          0.8f,  0.4f, 7907327},
    {"minecraft:taiga",           0.25f, 0.8f, 8233983},
    {"minecraft:ocean",           0.5f,  0.5f, 8103167},
    {"minecraft:windswept_hills", 0.2f,  0.3f, 8233727},
}};
static_assert(BIOMES[std::to_underlying(Biome::Taiga)].id == "minecraft:taiga");
static_assert(BIOMES[std::to_underlying(Biome::Hills)].id == "minecraft:windswept_hills");

enum class Block : uint8_t {
    Air, Stone, Dirt, Grass, Water, OakLog, OakLeaves, ShortGrass,
    Sand, SpruceLog, SpruceLeaves, Fern, Podzol, Gravel, Snow,
};

struct BlockPos {
    int x, y, z;
};

struct ChunkScratch {
    psram_ptr<Block> blocks = psram_alloc<Block>(NUM_SECTIONS * 4096);   // x + z*16 + (y - MIN_Y)*256
    psram_ptr<uint8_t> light = psram_alloc<uint8_t>(NUM_SECTIONS * 2048);
    PacketBuf sections{16384};

    bool ok() const { return blocks && light && sections.allocated(); }
};

void world_init(uint32_t seed);
uint32_t world_seed();
BlockPos world_spawn();
Biome world_biome_at(int x, int z);
int world_height_at(int x, int z);

void world_encode_chunk(PacketBuf& out, ChunkScratch& w, int cx, int cz);
