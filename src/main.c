/*
 * M3: stories260K inference on an ESP32-WROOM-32, console only.
 *
 * Weights are memory-mapped from the 'model' partition; the tokenizer is
 * compiled in as a byte array. Type a prompt in the serial monitor (or wait
 * 5 seconds for BOS-only generation).
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"

#define LLM_GENERATE_TOKENS 256

#include "llm.h"
#include "tok512_blob.h"

static const char *TAG = "app";

static void console_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0));
}

/* timeout_ms < 0 blocks until Enter. Returns the line length. */
static int read_prompt(char *buf, int max_len, int timeout_ms)
{
    int len = 0;
    TickType_t start = xTaskGetTickCount();
    while (timeout_ms < 0 ||
           (xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len > 0) {
                break;
            }
        } else if (c == 0x7f || c == 0x08) {
            if (len > 0) {
                len--;
                uart_write_bytes(UART_NUM_0, "\b \b", 3);
            }
        } else if (len < max_len - 1) {
            buf[len++] = (char)c;
            uart_write_bytes(UART_NUM_0, (const char *)&c, 1);
        }
    }
    buf[len] = '\0';
    uart_flush_input(UART_NUM_0); /* drop a trailing CR/LF */
    return len;
}

/* Compare the scalar kernel against ESP-DSP's ESP32 assembly dot product. */
static void benchmark_dotprod(const Transformer *t)
{
    static float x[192];
    for (int i = 0; i < 192; i++) {
        x[i] = 0.001f * (float)((i % 13) - 6);
    }

    const int dim = t->config.dim;          // 64
    const int hidden = t->config.hidden_dim; // 172
    const int reps64 = 500;
    const int reps172 = 100;
    volatile float sink = 0.0f;
    float acc;

    acc = 0.0f;
    int64_t t0 = esp_timer_get_time();
    for (int r = 0; r < reps64; r++) {
        for (int i = 0; i < dim; i++) {
            acc += llm_dotprod_scalar(t->weights.wq + i * dim, x, dim);
        }
    }
    int64_t t1 = esp_timer_get_time();

    acc = 0.0f;
    for (int r = 0; r < reps64; r++) {
        for (int i = 0; i < dim; i++) {
            acc += llm_dotprod_espdsp(t->weights.wq + i * dim, x, dim);
        }
    }
    int64_t t2 = esp_timer_get_time();

    double dots64 = (double)reps64 * dim;
    double scalar64 = (double)(t1 - t0) / dots64;
    double dsp64 = (double)(t2 - t1) / dots64;

    acc = 0.0f;
    int64_t t3 = esp_timer_get_time();
    for (int r = 0; r < reps172; r++) {
        for (int i = 0; i < hidden; i++) {
            acc += llm_dotprod_scalar(t->weights.w1 + i * dim, x, dim);
        }
    }
    int64_t t4 = esp_timer_get_time();

    acc = 0.0f;
    for (int r = 0; r < reps172; r++) {
        for (int i = 0; i < hidden; i++) {
            acc += llm_dotprod_espdsp(t->weights.w1 + i * dim, x, dim);
        }
    }
    int64_t t5 = esp_timer_get_time();

    double dots172 = (double)reps172 * hidden;
    double scalar172 = (double)(t4 - t3) / dots172;
    double dsp172 = (double)(t5 - t4) / dots172;

    sink = acc;
    (void)sink;

    ESP_LOGI(TAG, "dot64 : scalar %.3f us  esp-dsp %.3f us  (x%.2f)",
             scalar64, dsp64, scalar64 / dsp64);
    ESP_LOGI(TAG, "dot64 : scalar %.0f MFLOP/s  esp-dsp %.0f MFLOP/s",
             2.0 * dim / scalar64, 2.0 * dim / dsp64);
    ESP_LOGI(TAG, "row172: scalar %.3f us  esp-dsp %.3f us  (x%.2f)",
             scalar172, dsp172, scalar172 / dsp172);

    float a = llm_dotprod_scalar(t->weights.wq, x, dim);
    float b = llm_dotprod_espdsp(t->weights.wq, x, dim);
    ESP_LOGI(TAG, "dot check: scalar %.6f esp-dsp %.6f diff %.6f", a, b,
             a - b);
}

static void generation_done(float tokens_per_second)
{
    ESP_LOGI(TAG, "achieved %.2f tok/s", tokens_per_second);
    ESP_LOGI(TAG, "heap: free %" PRIu32 " B, largest %" PRIu32 " B",
             esp_get_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static const void *map_model_partition(size_t *out_size)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "model");
    if (part == NULL) {
        ESP_LOGE(TAG, "partition 'model' not found");
        return NULL;
    }

    const void *base = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &base, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return NULL;
    }

    *out_size = part->size;
    ESP_LOGI(TAG, "model: 0x%08" PRIx32 " (%" PRIu32 " bytes) mapped at %p",
             part->address, part->size, base);
    return base;
}

void app_main(void)
{
    printf("Hello world!\n");
    ESP_LOGI(TAG, "stories260K inference demo");

    console_uart_init();

    size_t model_size = 0;
    const void *model = map_model_partition(&model_size);
    if (model == NULL) {
        return;
    }

    Transformer transformer;
    build_transformer(&transformer, model, model_size);

    Tokenizer tokenizer;
    build_tokenizer(&tokenizer, tok512_bin, tok512_bin_len,
                    transformer.config.vocab_size);

    Sampler sampler;
    build_sampler(&sampler, transformer.config.vocab_size, 1.0f, 0.9f,
                  esp_random());

    benchmark_dotprod(&transformer);

    char prompt[128];
    ESP_LOGI(TAG, "type a prompt, or wait 5 s for BOS-only generation");
    int len = read_prompt(prompt, sizeof(prompt), 5000);
    ESP_LOGI(TAG, "generating from %s", len > 0 ? "prompt" : "BOS");
    /* Sliding-window generation: keep producing past the window size. */
    generate(&transformer, &tokenizer, &sampler, len > 0 ? prompt : NULL,
             LLM_GENERATE_TOKENS, generation_done);

    while (true) {
        ESP_LOGI(TAG, "prompt>");
        len = read_prompt(prompt, sizeof(prompt), -1);
        generate(&transformer, &tokenizer, &sampler, len > 0 ? prompt : NULL,
                 LLM_GENERATE_TOKENS, generation_done);
    }
}
