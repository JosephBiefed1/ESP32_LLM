/*
 * M1 verification firmware.
 *
 * Finds the 'model' partition, maps it read-only, and prints the model header
 * and a few sanity numbers. Replaced by the LLM demo in M2.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "m1";

typedef struct {
    int32_t dim;
    int32_t hidden_dim;
    int32_t n_layers;
    int32_t n_heads;
    int32_t n_kv_heads;
    int32_t vocab_size;
    int32_t seq_len;
} model_header_t;

void app_main(void)
{
    printf("Hello world!\n");
    ESP_LOGI(TAG, "M1 model-partition check");

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "model");
    if (part == NULL) {
        ESP_LOGE(TAG, "partition 'model' (data,0x40) not found");
        return;
    }
    ESP_LOGI(TAG, "partition: label='%s' offset=0x%08" PRIx32 " size=%" PRIu32,
             part->label, part->address, part->size);

    const void *base = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &base, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "mapped at %p (%" PRIu32 " bytes)", base, part->size);

    model_header_t h;
    memcpy(&h, base, sizeof(h));
    ESP_LOGI(TAG, "header: dim=%" PRId32 " hidden=%" PRId32 " layers=%" PRId32
                  " heads=%" PRId32 " kv_heads=%" PRId32 " vocab=%" PRId32
                  " seq=%" PRId32,
             h.dim, h.hidden_dim, h.n_layers, h.n_heads, h.n_kv_heads,
             h.vocab_size, h.seq_len);

    const model_header_t expected = {64, 172, 5, 8, 4, 512, 512};
    ESP_LOGI(TAG, "header %s",
             memcmp(&h, &expected, sizeof(h)) == 0 ? "OK" : "MISMATCH");

    const float *weights = (const float *)((const uint8_t *)base + sizeof(h));
    ESP_LOGI(TAG, "first weights: %.4f %.4f %.4f", weights[0], weights[1],
             weights[2]);
    ESP_LOGI(TAG, "free heap: %" PRIu32 " B, largest block: %" PRIu32 " B",
             esp_get_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "idle, free heap %" PRIu32 " B", esp_get_free_heap_size());
    }
}
