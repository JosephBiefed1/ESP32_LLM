# AGENTS.md

Orientation for AI agents and humans working in this repository.

## Working agreement (for agents)

- **Ask before changing anything.** Describe the change you intend to make and
  why, and wait for explicit approval before editing files or running commands
  that modify the project.
- **Summarize after changing.** Once an approved change is applied, report what
  was changed and why, plus any build/verification result.

## What this is

Firmware for an **ESP32-WROOM-32** (4 MB flash, `esp32dev`, **no PSRAM**) built
with **PlatformIO** using the **ESP-IDF** framework (not Arduino). The goal is
on-device inference with the 260 K-parameter `stories260K` model (TinyStories)
via a llama2.c port, with weights **memory-mapped from flash** and no OTA.

Current state: **M1** — the partition table, model assets, and a temporary
verification `app_main()` that maps the model and prints its header. The
inference port is M2.

## Layout

| Path | Purpose |
| --- | --- |
| `platformio.ini` | PlatformIO env: platform, board, framework, partitions file. |
| `partitions.csv` | Single-app layout + `model` data partition (subtype `0x40`). |
| `sdkconfig.defaults` | Applied when SDK config is generated (4 MB flash, custom table, `-O2`). |
| `sdkconfig.esp32dev` | **Generated**, gitignored. Delete it to re-apply `sdkconfig.defaults`. |
| `src/main.c` | Current entry point (M1 verifier). |
| `src/CMakeLists.txt` | Registers `src/**/*.c`; `INCLUDE_DIRS "llm"`. |
| `src/llm/` | llama2.c port (M2) + `tok512.bin` + generated `tok512_blob.h`. |
| `data/stories260K.bin` | Model weights, 1,056,540 B (committed). |
| `data/tok512.bin` | Tokenizer, 6,227 B (committed; canonical copy also under `src/llm/`). |
| `data/SHA256SUMS` | Hashes checked by `scripts/flash_model.sh`. |
| `scripts/flash_model.sh` | Writes the model into the `model` partition over USB. |
| `scripts/gen_tokenizer_blob.sh` | Regenerates `src/llm/tok512_blob.h` with `xxd`. |
| `archive/ota_demo_main.c` | Previous Wi-Fi OTA demo. Not compiled; kept for reference. |
| `.pio/` | Build output. **Generated — never edit or commit.** |

## Build / flash / monitor

```bash
pio run                           # compile
pio run -t upload                 # flash bootloader + table + app over USB
scripts/flash_model.sh            # flash model into 'model' partition
pio device monitor -b 115200      # serial console
pio run -t erase                  # full chip erase (also wipes the model!)
pio run -t clean                  # remove build artifacts
```

If `pio` is not on `PATH`, use `~/.platformio/penv/bin/pio`.

First deploy (after any partition-table change):

```bash
pio run -t upload
scripts/flash_model.sh            # PORT=/dev/ttyUSB0 if auto-detect fails
```

Later app-only updates: `pio run -t upload` is enough; the model partition is
untouched. The model is **not** part of `pio run -t upload`.

## Flash layout (no OTA)

| Partition | Type | Offset | Size |
| --- | --- | --- | --- |
| `nvs` | data/nvs | `0x9000` | 24 K |
| `phy_init` | data/phy | `0xf000` | 4 K |
| `factory` | app/factory | `0x10000` | 2 MiB |
| `model` | data/`0x40` | `0x210000` | 1.9375 MiB |

No `otadata` and no second app slot: normal OTA is impossible by design. A bad
build needs USB. The `model` partition is 64 KiB aligned, so
`esp_partition_mmap()` works directly on it.

## Model and runtime notes

- Header (first 28 bytes): `dim=64`, `hidden_dim=172`, `n_layers=5`,
  `n_heads=8`, `n_kv_heads=4`, `vocab_size=512`, `seq_len=512`.
- Weights are read through the flash cache; nothing is bulk-copied to RAM.
  This model is **dense**, so every token streams ~1.06 MB (unlike PLE designs
  that keep sparse tables in flash and touch a few rows per token).
- KV cache is the main RAM cost: `2 × 5 × seq_len × 64 × 4 B`
  (~82 KB at ctx 32, ~164 KB at ctx 64). Plan: ctx 64 with fallback to 32.
- Copy RMSNorm vectors (~2.5 KB) into internal SRAM at boot.
- Worker task stacks: 4 KB each. Yield (`vTaskDelay(1)`) once per token so the
  task watchdog keeps seeing the idle task on CPU1.
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (`-O2`); add `-ffast-math` for the
  scalar dot products. Flash is DIO/40 MHz for now; QIO/80 MHz is an M4 test.

## Conventions

- Language: C (C11 via ESP-IDF). Keep sources under `src/`; the CMake glob
  picks up every `.c` file automatically.
- Logging: `ESP_LOGx(TAG, ...)` with a `static const char *TAG`.
- Delays: `vTaskDelay(pdMS_TO_TICKS(ms))`; never busy-wait.
- Do not add Arduino code, Wi-Fi, or OTA to the inference build.
- Keep the model blob and tokenizer in sync; never mix files from different
  checkpoints.
- Commit the model binaries (decision: repo is self-contained). Use Git LFS if
  a future model approaches tens of MB.

## Gotchas

- No PSRAM on this module: ~320 KB DRAM total, ~280 KB free heap at boot.
  Watch `heap_caps_get_largest_free_block()` when changing context length.
- `sdkconfig.esp32dev` is generated. Delete it after editing
  `sdkconfig.defaults`, then rebuild.
- `pio run -t erase` removes the model; re-run `scripts/flash_model.sh`.
- Serial output is 115200 baud; the monitor must use the same speed.
- If the board does not enter download mode, hold **BOOT** while the upload
  starts.
- OTA demo is archived, not deleted; it is not compiled.

## Attribution

Model: `stories260K` from karpathy/tinyllamas, distributed via
DaveBben/esp32-llm (llama2.c port). TinyStories dataset: Eldan & Li,
arXiv:2305.07759. Check the upstream licenses before redistributing.

## Recent changes

- **2026-11-21 — M0:** added `data/stories260K.bin`, `data/tok512.bin`,
  `data/SHA256SUMS`, `src/llm/tok512.bin`; commit `3dde0fb`.
- **2026-11-21 — M1:** replaced the OTA table with a single-app layout +
  `model` partition; archived the OTA demo; added `scripts/flash_model.sh`
  and the tokenizer blob generator; M1 verifier in `src/main.c`; `-O2` via
  `CONFIG_COMPILER_OPTIMIZATION_PERF=y`.
- **2026-11-21:** `git init` + baseline commit `7086713` (working OTA demo).
