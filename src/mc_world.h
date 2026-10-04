#pragma once

#include "mc_packet.h"
#include <cstdint>

static constexpr int SEA_LEVEL = -52;
static constexpr int MIN_Y = -64;
static constexpr int NUM_SECTIONS = 24;

// Order matters: the index is the biome's registry id sent to the client
enum Biome : uint8_t { BIOME_PLAINS, BIOME_TAIGA, BIOME_OCEAN, BIOME_HILLS, BIOME_COUNT };

struct BiomeInfo {
    const char* id;
    float temperature;
    float downfall;
    int32_t sky_color;
};

extern const BiomeInfo BIOMES[BIOME_COUNT];

struct ChunkScratch {
    uint8_t* blocks = nullptr;   // palette index per block, x + z*16 + (y - MIN_Y)*256
    uint8_t* light = nullptr;
    PacketBuf sections;

    bool init();
    void free();
};

void world_init(uint32_t seed);
uint32_t world_seed();
void world_spawn(int& x, int& y, int& z);
Biome world_biome_at(int x, int z);
int world_height_at(int x, int z);

void world_encode_chunk(PacketBuf& out, ChunkScratch& w, int cx, int cz);
