#include <cstring>
#include <cstdio>
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "config.h"
#include "mc_types.h"
#include "mc_packet.h"
#include "mc_registry.h"
#include "mc_play.h"

static constexpr const char* TAG = "mc_server";

enum class ConnState { HANDSHAKE, STATUS, LOGIN, CONFIG, PLAY };

static volatile bool wifi_connected = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Connected on IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

static void wifi_init()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config{};
    std::memcpy(wifi_config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID));
    std::memcpy(wifi_config.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init done, connecting to \"%s\"", WIFI_SSID);
}

static void send_status_response(int sock, PacketBuf& out) {
    char json[256];
    snprintf(json, sizeof(json),
        "{\"version\":{\"name\":\"%s\",\"protocol\":%d},"
        "\"players\":{\"max\":%d,\"online\":0},"
        "\"description\":{\"text\":\"ESP32-S3 Minecraft Server\"}}",
        MC_VERSION_NAME, MC_PROTOCOL_VERSION, MC_MAX_PLAYERS);

    out.reset();
    pkt_write_varint(out, 0x00);
    pkt_write_string(out, json);
    out.send_packet(sock);
}

static void send_pong(int sock, PacketBuf& out, int64_t payload) {
    out.reset();
    pkt_write_varint(out, 0x01);
    pkt_write_i64(out, payload);
    out.send_packet(sock);
}

static void handle_client(int sock) {
    PacketBuf in, out;
    in.init();
    out.init();

    ConnState state = ConnState::HANDSHAKE;

    while (in.recv_packet(sock)) {
        int32_t packet_id = pkt_read_varint(in);

        if (state == ConnState::HANDSHAKE && packet_id == 0x00) {
            int32_t proto_ver = pkt_read_varint(in);
            char server_addr[256];
            pkt_read_string(in, server_addr, sizeof(server_addr));
            uint16_t server_port = pkt_read_u16(in);
            int32_t next_state = pkt_read_varint(in);

            ESP_LOGI(TAG, "Handshake: proto=%d addr=%s port=%d next=%d",
                     proto_ver, server_addr, server_port, next_state);

            if (next_state == 1) state = ConnState::STATUS;
            else if (next_state == 2) state = ConnState::LOGIN;
            continue;
        }

        if (state == ConnState::STATUS) {
            if (packet_id == 0x00) {
                ESP_LOGI(TAG, "Status request -> sending response");
                send_status_response(sock, out);
            } else if (packet_id == 0x01) {
                int64_t payload = pkt_read_i64(in);
                ESP_LOGI(TAG, "Ping -> pong");
                send_pong(sock, out, payload);
                break;
            }
            continue;
        }

        if (state == ConnState::LOGIN) {
            if (packet_id == 0x00) {
                char username[17];
                pkt_read_string(in, username, sizeof(username));
                uint64_t uuid_hi, uuid_lo;
                pkt_read_uuid(in, uuid_hi, uuid_lo);

                ESP_LOGI(TAG, "Login Start: user=%s", username);

                out.reset();
                pkt_write_varint(out, 0x02);
                pkt_write_uuid(out, uuid_hi, uuid_lo);
                pkt_write_string(out, username);
                pkt_write_varint(out, 0);
                out.send_packet(sock);

                ESP_LOGI(TAG, "Sent Login Success, waiting for ack");
            } else if (packet_id == 0x03) {
                ESP_LOGI(TAG, "Login Acknowledged -> Configuration state");
                state = ConnState::CONFIG;
                send_config_packets(sock, out);
            }
            continue;
        }

        if (state == ConnState::CONFIG) {
            if (packet_id == 0x03) {
                ESP_LOGI(TAG, "Client acknowledged config -> Play state");
                state = ConnState::PLAY;
                send_play_packets(sock, out);
                break;
            }
            continue;
        }
    }

    if (state == ConnState::PLAY) {
        int center_cx = 0, center_cz = 0;
        TickType_t last_ka = xTaskGetTickCount();
        PacketBuf scratch;
        scratch.init(8192);

        while (true) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            struct timeval tv = {1, 0};
            int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);

            if (ret > 0) {
                if (!in.recv_packet(sock)) break;
                int32_t pkt_id = pkt_read_varint(in);

                if (pkt_id == 0x1c || pkt_id == 0x1d) {
                    double px = pkt_read_f64(in);
                    pkt_read_f64(in);
                    double pz = pkt_read_f64(in);
                    int new_cx = static_cast<int>(floor(px)) >> 4;
                    int new_cz = static_cast<int>(floor(pz)) >> 4;

                    if (new_cx != center_cx || new_cz != center_cz) {
                        int old_cx = center_cx, old_cz = center_cz;
                        center_cx = new_cx;
                        center_cz = new_cz;
                        send_center_chunk(sock, out, new_cx, new_cz);
                        int vd = MC_VIEW_DISTANCE;
                        for (int cx = new_cx - vd; cx <= new_cx + vd; cx++)
                            for (int cz = new_cz - vd; cz <= new_cz + vd; cz++)
                                if (abs(cx - old_cx) > vd || abs(cz - old_cz) > vd)
                                    send_chunk(sock, out, scratch, cx, cz);
                    }
                }
            } else if (ret < 0) {
                break;
            }

            TickType_t now = xTaskGetTickCount();
            if ((now - last_ka) * portTICK_PERIOD_MS >= 10000) {
                out.reset();
                pkt_write_varint(out, 0x27);
                pkt_write_i64(out, static_cast<int64_t>(now));
                if (!out.send_packet(sock)) break;
                last_ka = now;
            }
        }

        scratch.free();
    }

    in.free();
    out.free();
}

static void tcp_server_task(void* pvParameters)
{
    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(MC_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    if (listen(listen_sock, 2) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Server listening on port %d", MC_PORT);

    while (true) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_sock < 0) {
            ESP_LOGE(TAG, "Accept failed errno %d", errno);
            continue;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "New connection from %s:%d", addr_str, ntohs(client_addr.sin_port));

        int nodelay = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        handle_client(client_sock);

        close(client_sock);
        ESP_LOGI(TAG, "Connection closed");
    }
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "ESP32-S3 Minecraft Server");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM: %u bytes", static_cast<unsigned>(psram_size));
    } else {
        ESP_LOGW(TAG, "No PSRAM detected");
    }

    ESP_LOGI(TAG, "Free heap: %u bytes", static_cast<unsigned>(esp_get_free_heap_size()));

    wifi_init();

    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 32768, nullptr, 5, nullptr, 0);
}
