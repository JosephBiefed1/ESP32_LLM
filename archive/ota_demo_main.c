/*
 * ESP32-WROOM-32 demo: Wi-Fi + OTA update over the local network.
 *
 * The board downloads a firmware image over HTTP and writes it to the
 * inactive OTA slot (ota_0 <-> ota_1), then reboots into it. If the new image
 * never calls esp_ota_mark_app_valid_cancel_rollback(), the bootloader rolls
 * back to the previous slot on the next reset.
 *
 * Trigger: press the BOOT button (GPIO0) while the app is running.
 *
 * NOTE: the first flash must be over USB. OTA can update an app, but it cannot
 * change the partition table, and the table in partitions.csv is what creates
 * the two OTA slots in the first place.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

/* ---- Edit these three lines ------------------------------------------- */

/* 2.4 GHz network only: the ESP32 has no 5 GHz radio. */
#define WIFI_SSID     "SINGTEL-XXH3"
#define WIFI_PASSWORD "56en5tfntf"

/*
 * The computer running the HTTP server, *not* localhost - from the ESP32's
 * point of view localhost is the ESP32 itself. Find your LAN IP with
 * `hostname -I` (Linux) or `ipconfig` (Windows).
 *
 * Serve the build output with:
 *   cd .pio/build/esp32dev && python3 -m http.server 8000
 */
#define OTA_URL "http://192.168.1.239:8000/firmware.bin"

/* ----------------------------------------------------------------------- */

/* Bump this before each OTA test so the monitor shows which image booted. */
#define APP_VERSION "v2"

#define OTA_TRIGGER_GPIO GPIO_NUM_0 /* BOOT button on most dev boards */

static const char *TAG = "app";

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\"...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void ota_update(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "OTA from %s -> partition '%s' (0x%08" PRIx32 ")",
             OTA_URL, next ? next->label : "?", next ? next->address : 0);

    esp_http_client_config_t http_cfg = {
        .url = OTA_URL,
        .timeout_ms = 10000,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return;
    }

    while (true) {
        err = esp_https_ota_perform(ota);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        ESP_LOGI(TAG, "Downloaded %d / %d bytes",
                 esp_https_ota_get_image_len_read(ota),
                 esp_https_ota_get_image_size(ota));
    }

    if (!esp_https_ota_is_complete_data_received(ota)) {
        ESP_LOGE(TAG, "OTA download incomplete (%s)", esp_err_to_name(err));
        esp_https_ota_abort(ota);
        return;
    }

    err = esp_https_ota_finish(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "OTA done, rebooting into the new image...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void app_main(void)
{
    printf("Hello world!\n");
    ESP_LOGI(TAG, "ESP32 OTA demo %s booting", APP_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running %s from '%s' (0x%08" PRIx32 ")",
             APP_VERSION, running->label, running->address);

    /* Confirms an OTA-updated image so the bootloader does not roll it back.
     * Move this after your own health checks in real firmware. */
    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not mark app valid: %s", esp_err_to_name(err));
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << OTA_TRIGGER_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    if (wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection failed; OTA unavailable");
        return;
    }

    ESP_LOGI(TAG, "Ready (%s). Press BOOT to fetch %s", APP_VERSION, OTA_URL);

    bool was_pressed = false;
    uint32_t tick = 0;
    while (true) {
        bool pressed = (gpio_get_level(OTA_TRIGGER_GPIO) == 0);
        if (pressed && !was_pressed) {
            ESP_LOGI(TAG, "BOOT pressed, starting OTA");
            ota_update();
            ESP_LOGI(TAG, "Back to idle");
        }
        was_pressed = pressed;

        if (++tick % 50 == 0) {
            ESP_LOGI(TAG, "alive, free heap %" PRIu32 " bytes",
                     esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
