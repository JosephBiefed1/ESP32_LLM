# ESP32-LLM on an ESP32-WROOM-32

On-device text generation with a **260 K-parameter** TinyStories model
(`stories260K`, llama2.c) on a plain **ESP32-WROOM-32**: 4 MB flash, **no
PSRAM**, no cloud, no OTA. Weights are memory-mapped from a flash partition and
streamed through the cache during inference.

**Measured: 11.1 tokens/s** (64-token generations, 240 MHz, QIO flash @ 80 MHz).

## Hardware and flash layout

| Partition    | Type          | Offset       | Size       |
| ------------ | ------------- | ------------ | ---------- |
| `nvs`      | data/nvs      | `0x9000`   | 24 KiB     |
| `phy_init` | data/phy      | `0xf000`   | 4 KiB      |
| `factory`  | app           | `0x10000`  | 2 MiB      |
| `model`    | data/`0x40` | `0x210000` | 1.9375 MiB |

- Model file: 1,056,540 B (fp32), flashed once at `0x210000`.
- Tokenizer: 512-token vocab, 6,227 B, compiled into the app.
- No `otadata` and no second app slot: updates are USB-only.
- App image: 178,891 B (8.5% of the 2 MiB partition).

## Build, flash, run

```bash
# 1. App (bootloader + partition table + app) over USB
~/.platformio/penv/bin/pio run -t upload

# 2. Model blob into the 'model' partition (offset read from partitions.csv)
scripts/flash_model.sh            # PORT=/dev/ttyUSB0 if auto-detect fails

# 3. Console
~/.platformio/penv/bin/pio device monitor -b 115200
```

Type a prompt within 5 seconds, or wait for BOS-only generation. After each
generation the firmware prints `prompt>` and waits for another line.

The model is **not** part of `pio run -t upload`; `pio run -t erase` wipes it.

## Results

All runs: 64 generated tokens, context 64, temperature 1.0, top-p 0.9, greedy
timing excludes the first token, serial console attached. Dot-product timings
are per call, measured with `esp_timer` over 500×64 and 100×172 rows of the
actual model weights.

### Kernel: hand-written scalar vs ESP-DSP (ESP32 `ae32` assembly)

| Flash mode | Dot len                    | Scalar                           | ESP-DSP                | Winner         |
| ---------- | -------------------------- | -------------------------------- | ---------------------- | -------------- |
| DIO 40 MHz | 64 (cache-resident)        | **1.376 µs** (93 MFLOP/s) | 1.526 µs (84 MFLOP/s) | scalar, 1.10× |
| DIO 40 MHz | 64, 172 rows (flash-bound) | **25.98 µs**              | 26.07 µs              | tie            |
| QIO 80 MHz | 64 (cache-resident)        | **1.50 µs** (85 MFLOP/s)  | 1.53 µs (84 MFLOP/s)  | scalar, 1.02× |
| QIO 80 MHz | 64, 172 rows (flash-bound) | **15.40 µs**              | 15.50 µs              | tie            |

End-to-end with ESP-DSP in the matmul: **6.67 tok/s**, identical to the scalar
build. Conclusion: **ESP-DSP's `dsps_dotprod_f32_ae32` gives no benefit here**;
the hand-written 4-accumulator C kernel is equal or slightly faster, and it
keeps the build dependency-free. The firmware defaults to the scalar kernel;
ESP-DSP is vendored only for the benchmark (`LLM_USE_ESPDSP=1` switches).

### Configuration impact (end-to-end)

| Config                        | tok/s           | Relative         |
| ----------------------------- | --------------- | ---------------- |
| 160 MHz, DIO 40 MHz (initial) | 6.42            | 1.00×           |
| 240 MHz, DIO 40 MHz           | 6.67            | 1.04×           |
| 240 MHz, QIO 80 MHz           | **11.11** | **1.73×** |

The CPU clock barely matters; **flash read bandwidth dominates**. Moving from
2 data lines at 40 MHz to 4 lines at 80 MHz cut the flash-bound dot from
25.98 µs to 15.40 µs and raised throughput 67%. This model is dense, so every
token streams all ~1 MB of weights from flash.

### Memory

| Item                                                   | Size        |
| ------------------------------------------------------ | ----------- |
| Weights (flash-mapped, not RAM)                        | 1,056,540 B |
| KV cache (2 × 5 layers × 64 ctx × 32 kv_dim × 4 B) | 81,920 B    |
| Free heap after generation                             | 176,004 B   |
| Largest free block                                     | 110,592 B   |

Context is clamped from the trained 512 to **64** to keep the KV cache inside
the 320 KiB DRAM. A 32-token context halves that again if needed.

## Implementation notes

- Weights point directly into `esp_partition_mmap()`'d flash; nothing is copied
  to RAM. Rows are streamed sequentially, which is optimal for the flash cache.
- Matmul is `(out, in)` row-major: `y[o] = Σ_k x[k]·W[o][k]`, four independent
  accumulators, `-O2 -ffast-math`. No transpose: the file layout is already
  row-major per output; transposing at runtime would read 1 MB per token twice.
- RMSNorm vectors are small and reused every token.
- `v4sf` is a plain `float`. It originally carried `aligned(16)`, which changed
  the calling convention for `float` parameters and corrupted `Sampler` fields
  (`probindex`, `topp`), causing intermittent crashes. Removing the attribute
  fixed it.
- Context is clamped before `malloc_run_state`, so KV offsets and the cache size
  always agree.
- The inference task yields once per token so the task watchdog still sees the
  idle task on CPU1.
- Console input uses the UART0 driver; prompts are echoed and capped at 127
  bytes.

## Limitations and next steps

- TinyStories-quality output, 512-token vocabulary, 64-token context.
- No Wi-Fi/OTA: `pio run -t upload` for every update (the archived OTA demo is
  in `archive/ota_demo_main.c`).
- int8/int4 weights would cut flash traffic ~4×; that is the next real speed
  lever. PSRAM (WROVER/S3) would allow larger contexts and staging.

## Attribution

- Model: `stories260K` from
  [karpathy/tinyllamas](https://huggingface.co/karpathy/tinyllamas), distributed
  with [DaveBben/esp32-llm](https://github.com/DaveBben/esp32-llm) (llama2.c
  port). TinyStories: Eldan & Li, arXiv:2305.07759.
- ESP-DSP `dsps_dotprod_f32_ae32.S` (benchmark only): Apache-2.0, Espressif.
- llama2.c reference: Andrej Karpathy.
