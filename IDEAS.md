# IDEAS / deferred work

Backlog for future sessions. **Read `AGENTS.md` first.** Nothing here is
approved for implementation: propose the change, get explicit approval, then
act.

Status: `open` · `blocked` · `done`

## P0 — finish and verify the current build

### 1. Flash and verify the sliding-window fix — `blocked`
- `src/llm/llm.c` now uses a 128-entry ring KV cache and `LLM_GENERATE_TOKENS`
  = 256.
- Fixed `att[t]` → `att[t - first]`; the old index overflowed each head's
  128-float slice after position 128, corrupting heap (junk tokens, then Guru).
- Build is green, **not flashed**: `/dev/ttyACM0` was held by the monitor.
- Acceptance: 256-token run, no Guru, sane `largest free block` (~28 KB),
  `heap: free` ~92 KB, coherent through the 128 wrap, record tok/s.
- Then update `README.md` results (context 128, sliding window) and AGENTS.

### 2. Monitor reset lines — `done` (needs restart to take effect)
- `platformio.ini` has `monitor_speed = 115200`, `monitor_dtr = 0`,
  `monitor_rts = 0`.
- The CH340 board wires RTS→EN, DTR→GPIO0; asserting them holds the ESP32 in
  reset, which looked like a "stuck" monitor.

## P1 — generation UX

### 3. Endless output + stop key — `open`
- Remove the `if (next == 1) break;` end-of-story break and use a very large
  `steps`; the ring window keeps RAM flat.
- Poll UART inside the token loop and stop on `q`/Ctrl-C, or on a GPIO.
- Trade-off: after the window rotates, coherence drifts; no natural ending.

### 4. Auto-continue / prompt chaining — `open`
- Alternative to (3): when `generate()` returns, re-feed the last ~64 tokens
  as the next prompt. Visible continuity, faster drift, simpler code.

### 5. Sentence-aware ending and truncation marker — `open`
- If finite 256-token runs are kept, break on `.`/`!`/`?` after a minimum, and
  print `…` when the budget is the reason for stopping.

### 6. Prompt diagnostics — `open`
- Print prompt token count and remaining output budget; reject prompts longer
  than the window.

## P2 — performance

### 7. int8 weights — `open`
- Biggest remaining win: 4× less flash traffic (~1 MB → ~265 KB per token).
- Needs a quantizer script + accuracy check; then a per-row int8 dot with
  int32 accumulation. Expect roughly 2–4× throughput at the same context.

### 8. int8 KV cache — `open`
- Halves the KV footprint, enabling context 192–256 or larger models.

### 9. int4 weights — `open`
- 8× less flash than fp32, more accuracy risk; only worth it if int8 is not
  enough.

### 10. Dual-core matmul — `open`
- The original repo splits rows across both cores. Currently scalar on one
  core; worth measuring now that flash is QIO 80 MHz.

## P3 — larger models

### 11. A second hardware target: ESP32-S3 + PSRAM — `open`
- N16R8 (16 MB flash, 8 MB PSRAM, SIMD) runs 8–29 M 4-bit models at ~9.5 tok/s
  (see slvDev/esp32-ai). Would need a separate PlatformIO env and this project
  kept as the no-PSRAM baseline.

### 12. PLE / sparse embedding tables — `open`
- The only way to host tens of millions of parameters: most params live in a
  flash lookup table, a few rows read per token. Requires **training** a model
  with that architecture; cannot be retrofitted to `stories260K`.

### 13. Cloud LLM client — `open`
- For useful Q&A, send prompts over Wi-Fi to a hosted model. Reuses the archived
  OTA demo's Wi-Fi setup; costs ~50–70 KB heap.

### 14. Custom tokenizer / larger vocab — `open`
- A 32K vocab improves text quality but adds `vocab × dim` embedding and output
  head params, plus a dense softmax per token.

### 15. Logit-reading closed-choice mode (mini-Jev idea) — `open`
- Instead of generating text, run one forward pass and compare the logits of
  candidate tokens (e.g. `A`/`B`/`C`, `yes`/`no`, or whole option names).
- The mini-Jev study (r-ms/mini-jev) shows this matches JSON generation for
  closed choices on Qwen3-4B, with no decode cost. The useful part here is the
  **interface**, not the 4B model: one pass (~90 ms) instead of 6–23 s of
  generation, and no chance of drifting/garbling.
- On this board: expose `s->logits` from `forward()`, prefill a prompt once
  (shared KV prefix), then read the scores for N candidate token IDs.
- Constraints: candidates must exist as tokens in the 512-word vocab; the
  model is TinyStories-only, so treat it as a toy classifier (story tone,
  yes/no, A/B/C), not a general instruction model.

### 16. FPGA co-processor — `open`
- An FPGA can accelerate matmul, but only if it also **owns the weights**.
  Shipping 1 MB of weights over SPI per token (~10–40 MB/s) would be slower
  than the current 90 ms/token. Useful split: FPGA holds the model + KV and
  runs the forward pass; ESP32 does tokenization/UI and sends one activation
  vector, receiving logits.
- Requires: FPGA with DSP slices + external SRAM/DDR (or int8 weights in
  BRAM), SPI slave protocol, RTL matmul/systolic array, and a driver on the
  ESP32. Weeks-to-months of work; a PSRAM/SIMD MCU or a small SBC is far
  cheaper for the same result.
- A 4B model is out of reach on a hobby FPGA: ~2 GB at 4-bit, plus KV, needs
  large DDR and a high-end device.

## Hard limits on a bigger model (measured on this board)

| Resource | Value | Consequence |
| --- | --- | --- |
| Flash | 4 MiB (model partition 1.9375 MiB) | fp32 ≤ ~0.5 M params; int8 ≤ ~2 M; int4 ≤ ~4 M in the current table |
| DRAM | 320 KB, ~285 KB free at boot | KV + activations only; no PSRAM |
| KV at ctx 128 | 163,840 B for this model | free heap ~92 KB; larger dims/layers explode |
| Flash-bound dot | ~15.4 µs per 64-wide row (QIO 80) | ~8.3 MFLOP/s effective when streaming |
| Compute | ~93 MFLOP/s cache-resident | only matters once weights are quantized |
| Flash address space | 4 MB data mmap on ESP32 | model mapping shares space with app rodata |
| SIMD | none on original ESP32 | scalar FPU only; S3 has SIMD |

Practical envelope for a **dense** llama2-style model here:

- 264 K fp32 (current): 11.1 tok/s, ctx 128.
- ~500 K fp32: ~5–6 tok/s, ctx 64–128.
- ~1 M int8: ~10–15 tok/s, ctx 32–64, needs training/quantization.
- ≥3 M dense: flash may fit, but per-token streaming and KV make it
  impractical (well under 5 tok/s, tiny context).

Bigger footprints need PSRAM and/or a sparse architecture, not just a larger
partition.

## Notes for future agents

- Do not resize the model partition or app without a USB reflash; OTA is gone.
- `pio run -t erase` wipes the model; re-run `scripts/flash_model.sh`.
- Keep `v4sf` a plain `float`; `aligned(16)` corrupted the float-arg ABI.
- Re-run the dot benchmark after any kernel change; scalar beat ESP-DSP `ae32`.
- Update `README.md` results and this file when something moves to `done`.
