/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _LHDCV5BT_H_
#define _LHDCV5BT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "lhdcv5_api.h"

int32_t lhdcv5BT_free_handle(HANDLE_LHDC_BT handle);

int32_t lhdcv5BT_get_handle(uint32_t version, HANDLE_LHDC_BT *handle);

int32_t lhdcv5BT_get_bitrate(HANDLE_LHDC_BT handle, uint32_t *bitrate);

int32_t lhdcv5BT_set_bitrate(HANDLE_LHDC_BT handle, uint32_t bitrate_inx);

int32_t lhdcv5BT_set_max_bitrate(HANDLE_LHDC_BT handle, uint32_t max_bitrate_inx);

int32_t lhdcv5BT_set_min_bitrate(HANDLE_LHDC_BT handle, uint32_t min_bitrate_inx);

int32_t lhdcv5BT_adjust_bitrate(HANDLE_LHDC_BT handle, uint32_t queueLen);

int32_t lhdcv5BT_init_encoder(HANDLE_LHDC_BT handle, uint32_t sampling_freq,
                              uint32_t bits_per_sample, uint32_t bitrate_inx, uint32_t mtu,
                              uint32_t interval, uint32_t reserved);

int32_t lhdcv5BT_get_block_Size(HANDLE_LHDC_BT handle, uint32_t *samples_per_frame);

int32_t lhdcv5BT_encode(HANDLE_LHDC_BT handle, void *p_in_pcm, uint32_t pcm_bytes,
                        uint8_t *p_out_buf, uint32_t out_buf_bytes, uint32_t *p_out_bytes,
                        uint32_t *p_out_frames);

#ifdef __cplusplus
}
#endif

#endif /* _LHDCV5BT_H_ */
