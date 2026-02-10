#include <cstring>
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

static constexpr const char* TAG = "mc_server";

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

    ESP_LOGI(TAG, "WiFi init done, connecting to \"%s\"...", WIFI_SSID);
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

        // For now: read and hex-dump incoming data
        uint8_t buf[256];
        while (true) {
            int len = recv(client_sock, buf, sizeof(buf), 0);
            if (len <= 0) {
                if (len == 0) {
                    ESP_LOGI(TAG, "Client disconnected");
                } else {
                    ESP_LOGE(TAG, "Recv error: errno %d", errno);
                }
                break;
            }

            char hex[32 * 3 + 1];
            int dump_len = len < 32 ? len : 32;
            for (int i = 0; i < dump_len; i++) {
                sprintf(hex + i * 3, "%02x ", buf[i]);
            }
            hex[dump_len * 3] = '\0';
            ESP_LOGI(TAG, "Received %d bytes: %s%s", len, hex, len > 32 ? "." : "");
        }

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

    xTaskCreatePinnedToCore(tcp_server_task, "tcp_server", 8192, nullptr, 5, nullptr, 0);
}
