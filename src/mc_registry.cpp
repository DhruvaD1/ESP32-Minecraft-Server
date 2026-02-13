#include "mc_registry.h"
#include "mc_types.h"
#include "mc_nbt.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "mc_registry";

struct DamageType {
    const char* id;
    const char* msg;
    const char* scaling;
    float exhaustion;
};

static const DamageType DAMAGE_TYPES[] = {
    {"minecraft:arrow",              "arrow",            "when_caused_by_living_non_player", 0.1f},
    {"minecraft:bad_respawn_point",  "badRespawnPoint",  "always",                           0.1f},
    {"minecraft:cactus",             "cactus",           "when_caused_by_living_non_player", 0.1f},
    {"minecraft:campfire",           "inFire",           "when_caused_by_living_non_player", 0.1f},
    {"minecraft:cramming",           "cramming",         "when_caused_by_living_non_player", 0.0f},
    {"minecraft:dragon_breath",      "dragonBreath",     "when_caused_by_living_non_player", 0.0f},
    {"minecraft:drown",              "drown",            "when_caused_by_living_non_player", 0.0f},
    {"minecraft:dry_out",            "dryout",           "when_caused_by_living_non_player", 0.1f},
    {"minecraft:ender_pearl",        "fall",             "when_caused_by_living_non_player", 0.0f},
    {"minecraft:explosion",          "explosion",        "always",                           0.1f},
    {"minecraft:fall",               "fall",             "when_caused_by_living_non_player", 0.0f},
    {"minecraft:falling_anvil",      "anvil",            "when_caused_by_living_non_player", 0.1f},
    {"minecraft:falling_block",      "fallingBlock",     "when_caused_by_living_non_player", 0.1f},
    {"minecraft:falling_stalactite", "fallingStalactite","when_caused_by_living_non_player", 0.1f},
    {"minecraft:fireball",           "fireball",         "when_caused_by_living_non_player", 0.1f},
    {"minecraft:fireworks",          "fireworks",        "when_caused_by_living_non_player", 0.1f},
    {"minecraft:fly_into_wall",      "flyIntoWall",      "when_caused_by_living_non_player", 0.0f},
    {"minecraft:freeze",             "freeze",           "when_caused_by_living_non_player", 0.0f},
    {"minecraft:generic",            "generic",          "when_caused_by_living_non_player", 0.0f},
    {"minecraft:generic_kill",       "genericKill",      "when_caused_by_living_non_player", 0.0f},
    {"minecraft:hot_floor",          "hotFloor",         "when_caused_by_living_non_player", 0.1f},
    {"minecraft:in_fire",            "inFire",           "when_caused_by_living_non_player", 0.1f},
    {"minecraft:in_wall",            "inWall",           "when_caused_by_living_non_player", 0.0f},
    {"minecraft:indirect_magic",     "indirectMagic",    "when_caused_by_living_non_player", 0.0f},
    {"minecraft:lava",               "lava",             "when_caused_by_living_non_player", 0.1f},
    {"minecraft:lightning_bolt",     "lightningBolt",    "when_caused_by_living_non_player", 0.1f},
    {"minecraft:mace_smash",         "mace_smash",       "when_caused_by_living_non_player", 0.1f},
    {"minecraft:magic",              "magic",            "when_caused_by_living_non_player", 0.0f},
    {"minecraft:mob_attack",         "mob",              "when_caused_by_living_non_player", 0.1f},
    {"minecraft:mob_attack_no_aggro","mob",              "when_caused_by_living_non_player", 0.1f},
    {"minecraft:mob_projectile",     "mob",              "when_caused_by_living_non_player", 0.1f},
    {"minecraft:on_fire",            "onFire",           "when_caused_by_living_non_player", 0.0f},
    {"minecraft:out_of_world",       "outOfWorld",       "when_caused_by_living_non_player", 0.0f},
    {"minecraft:outside_border",     "outsideBorder",    "when_caused_by_living_non_player", 0.0f},
    {"minecraft:player_attack",      "player",           "when_caused_by_living_non_player", 0.1f},
    {"minecraft:player_explosion",   "explosion.player", "always",                           0.1f},
    {"minecraft:sonic_boom",         "sonic_boom",       "always",                           0.0f},
    {"minecraft:spit",               "mob",              "when_caused_by_living_non_player", 0.1f},
    {"minecraft:stalagmite",         "stalagmite",       "when_caused_by_living_non_player", 0.0f},
    {"minecraft:starve",             "starve",           "when_caused_by_living_non_player", 0.0f},
    {"minecraft:sting",              "sting",            "when_caused_by_living_non_player", 0.1f},
    {"minecraft:sweet_berry_bush",   "sweetBerryBush",   "when_caused_by_living_non_player", 0.1f},
    {"minecraft:thorns",             "thorns",           "when_caused_by_living_non_player", 0.1f},
    {"minecraft:thrown",             "thrown",            "when_caused_by_living_non_player", 0.1f},
    {"minecraft:trident",            "trident",          "when_caused_by_living_non_player", 0.1f},
    {"minecraft:unattributed_fireball","onFire",          "when_caused_by_living_non_player", 0.1f},
    {"minecraft:wind_charge",        "mob",              "when_caused_by_living_non_player", 0.1f},
    {"minecraft:wither",             "wither",           "when_caused_by_living_non_player", 0.0f},
    {"minecraft:wither_skull",       "witherSkull",      "when_caused_by_living_non_player", 0.1f},
};

static constexpr int DAMAGE_TYPE_COUNT = sizeof(DAMAGE_TYPES) / sizeof(DAMAGE_TYPES[0]);

static void send_dimension_type(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x07);
    pkt_write_string(out, "minecraft:dimension_type");
    pkt_write_varint(out, 1);

    pkt_write_string(out, "minecraft:overworld");
    pkt_write_bool(out, true);
    nbt_begin(out);
    nbt_byte(out, "has_skylight", 1);
    nbt_byte(out, "has_ceiling", 0);
    nbt_byte(out, "ultrawarm", 0);
    nbt_byte(out, "natural", 1);
    nbt_double(out, "coordinate_scale", 1.0);
    nbt_byte(out, "bed_works", 1);
    nbt_byte(out, "respawn_anchor_works", 0);
    nbt_int(out, "min_y", -64);
    nbt_int(out, "height", 384);
    nbt_int(out, "logical_height", 384);
    nbt_string(out, "infiniburn", "#minecraft:infiniburn_overworld");
    nbt_string(out, "effects", "minecraft:overworld");
    nbt_float(out, "ambient_light", 0.0f);
    nbt_byte(out, "piglin_safe", 0);
    nbt_byte(out, "has_raids", 1);
    nbt_int(out, "monster_spawn_light_level", 0);
    nbt_int(out, "monster_spawn_block_light_limit", 0);
    nbt_end(out);

    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent dimension_type");
}

static void send_biome(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x07);
    pkt_write_string(out, "minecraft:worldgen/biome");
    pkt_write_varint(out, 1);

    pkt_write_string(out, "minecraft:plains");
    pkt_write_bool(out, true);
    nbt_begin(out);
    nbt_byte(out, "has_precipitation", 1);
    nbt_float(out, "temperature", 0.8f);
    nbt_float(out, "downfall", 0.4f);
    nbt_compound(out, "effects");
    nbt_int(out, "sky_color", 7907327);
    nbt_int(out, "fog_color", 12638463);
    nbt_int(out, "water_color", 4159204);
    nbt_int(out, "water_fog_color", 329011);
    nbt_end(out);
    nbt_end(out);

    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent biome");
}

static void send_chat_type(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x07);
    pkt_write_string(out, "minecraft:chat_type");
    pkt_write_varint(out, 1);

    pkt_write_string(out, "minecraft:chat");
    pkt_write_bool(out, true);

    const char* params[] = {"sender", "content"};

    nbt_begin(out);

    nbt_compound(out, "chat");
    nbt_string(out, "translation_key", "chat.type.text");
    nbt_string_list(out, "parameters", params, 2);
    nbt_end(out);

    nbt_compound(out, "narration");
    nbt_string(out, "translation_key", "chat.type.text.narrate");
    nbt_string_list(out, "parameters", params, 2);
    nbt_end(out);

    nbt_end(out);

    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent chat_type");
}

static void send_damage_type(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x07);
    pkt_write_string(out, "minecraft:damage_type");
    pkt_write_varint(out, DAMAGE_TYPE_COUNT);

    for (int i = 0; i < DAMAGE_TYPE_COUNT; i++) {
        pkt_write_string(out, DAMAGE_TYPES[i].id);
        pkt_write_bool(out, true);
        nbt_begin(out);
        nbt_string(out, "message_id", DAMAGE_TYPES[i].msg);
        nbt_string(out, "scaling", DAMAGE_TYPES[i].scaling);
        nbt_float(out, "exhaustion", DAMAGE_TYPES[i].exhaustion);
        nbt_end(out);
    }

    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent damage_type (%d entries)", DAMAGE_TYPE_COUNT);
}

static void send_painting_variant(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x07);
    pkt_write_string(out, "minecraft:painting_variant");
    pkt_write_varint(out, 1);

    pkt_write_string(out, "minecraft:kebab");
    pkt_write_bool(out, true);
    nbt_begin(out);
    nbt_string(out, "asset_id", "minecraft:kebab");
    nbt_int(out, "width", 1);
    nbt_int(out, "height", 1);
    nbt_end(out);

    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent painting_variant");
}

static void send_wolf_variant(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x07);
    pkt_write_string(out, "minecraft:wolf_variant");
    pkt_write_varint(out, 1);

    pkt_write_string(out, "minecraft:pale");
    pkt_write_bool(out, true);
    nbt_begin(out);
    nbt_string(out, "wild_texture", "minecraft:entity/wolf/wolf");
    nbt_string(out, "tame_texture", "minecraft:entity/wolf/wolf_tame");
    nbt_string(out, "angry_texture", "minecraft:entity/wolf/wolf_angry");
    nbt_string(out, "biomes", "minecraft:plains");
    nbt_end(out);

    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent wolf_variant");
}

static void send_empty_registry(int sock, PacketBuf& out, const char* id) {
    out.reset();
    pkt_write_varint(out, 0x07);
    pkt_write_string(out, id);
    pkt_write_varint(out, 0);
    out.send_packet(sock);
}

void send_config_packets(int sock, PacketBuf& out) {
    out.reset();
    pkt_write_varint(out, 0x0E);
    pkt_write_varint(out, 0);
    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent Known Packs (0 entries)");

    send_dimension_type(sock, out);
    send_biome(sock, out);
    send_chat_type(sock, out);
    send_damage_type(sock, out);
    send_painting_variant(sock, out);
    send_wolf_variant(sock, out);

    send_empty_registry(sock, out, "minecraft:trim_pattern");
    send_empty_registry(sock, out, "minecraft:trim_material");
    send_empty_registry(sock, out, "minecraft:banner_pattern");
    send_empty_registry(sock, out, "minecraft:enchantment");
    send_empty_registry(sock, out, "minecraft:jukebox_song");
    send_empty_registry(sock, out, "minecraft:instrument");

    ESP_LOGI(TAG, "All registries sent");

    out.reset();
    pkt_write_varint(out, 0x0C);
    pkt_write_varint(out, 1);
    pkt_write_string(out, "minecraft:vanilla");
    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent Feature Flags");

    out.reset();
    pkt_write_varint(out, 0x03);
    out.send_packet(sock);
    ESP_LOGI(TAG, "Sent Finish Configuration");
}
