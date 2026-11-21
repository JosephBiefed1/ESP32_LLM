#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-DSP float dot product, ESP32 assembly implementation (Apache-2.0).
 * dest = sum(src1[i] * src2[i]), i in [0, len). */
esp_err_t dsps_dotprod_f32_ae32(const float *src1, const float *src2,
                                float *dest, int len);

#ifdef __cplusplus
}
#endif
