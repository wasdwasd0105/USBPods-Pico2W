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

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum {
  LHDC_SAMPLE_FRAME_2P5MS_96000KHZ = 240,
  LHDC_SAMPLE_FRAME_5MS_44100KHZ = 240,
  LHDC_SAMPLE_FRAME_5MS_48000KHZ = 240,
  LHDC_SAMPLE_FRAME_5MS_96000KHZ = 480,
  LHDC_SAMPLE_FRAME_5MS_192000KHZ = 960,
  LHDC_SAMPLE_FRAME_10MS_44100KHZ = 480,
  LHDC_SAMPLE_FRAME_10MS_48000KHZ = 480,
  LHDC_MAX_SAMPLE_FRAME = 1920,
} LHDC_SAMPLE_FRAME_T;

typedef enum {
  LogLevelError = 1,
  LogLevelWarn = 2,
  LogLevelInfo = 3,
  LogLevelDebug = 4,
  LogLevelTrace = 5,
} LOGLEVEL_T;

typedef struct lhdc_cb_t lhdc_cb_t;

typedef int32_t STATUS_LHDC_BT;

typedef struct lhdc_cb_t *HANDLE_LHDC_BT;

typedef unsigned int HeaderInfoIndex;

typedef int lhdc_enc_error;

typedef unsigned int lhdc_enc_workspace_mode_options;

typedef unsigned int __LHDC_SAMPLE_FREQ__;

typedef unsigned int __LHDCBT_SMPL_FMT__;

typedef unsigned int __LHDC_FRAME_DURATION__;

typedef unsigned int __LHDC_ENC_INTERVAL__;

typedef unsigned int __LHDC_QUALITY__;

typedef unsigned int __LHDC_MTU_SIZE__;

typedef unsigned int __LHDC_VERSION__;

typedef unsigned int __LHDC_ENC_TYPE__;

typedef unsigned int __LHDC_LOG_LEVEL__;

typedef int __LHDC_FUNC_RET__;

typedef uint32_t SegmentRate;

#define ALL_HEADER_INFO_NUM 6

#define META_INDEX 5

#define LARC_INDEX 4

#define AR_INDEX 3

#define JAS_INDEX 2

#define VERSION_INDEX 1

#define ENC_SIZE_INDEX 0

#define LHDC_ENC_ERROR -1

#define LHDC_ENC_OK 0

#define LHDC_ENC_MODE_OPTION_0 0

#define LHDC_LOGMGR_LEVEL_DEBUG_NO_LOG 256

#define LHDC_LOGMGR_LEVEL_MAX 135

#define LHDC_LOGMGR_LEVEL_DEBUG_INTERNAL 128

#define LHDC_LOGMGR_LEVEL_DEBUG 7

#define LHDC_LOGMGR_LEVEL_INFO 6

#define LHDC_LOGMGR_LEVEL_NOTICE 5

#define LHDC_LOGMGR_LEVEL_WARNING 4

#define LHDC_LOGMGR_LEVEL_ERROR 3

#define LHDC_LOGMGR_LEVEL_CRIT 2

#define LHDC_LOGMGR_LEVEL_ALERT 1

#define LHDC_LOGMGR_LEVEL_EMERG 0

#define LHDC_SR_192000HZ 192000

#define LHDC_SR_96000HZ 96000

#define LHDC_SR_48000HZ 48000

#define LHDC_SR_44100HZ 44100

#define LHDCBT_SMPL_FMT_S24 24

#define LHDCBT_SMPL_FMT_S16 16

#define LHDC_FRAME_5MS 50

#define LHDC_ENC_INTERVAL_20MS 20

#define LHDC_ENC_INTERVAL_10MS 10

#define LHDC_QUALITY_INVALID 130

#define LHDC_QUALITY_CTRL_END 129

#define LHDC_QUALITY_CTRL_RESET_ABR 128

#define LHDC_QUALITY_UNLIMIT 14

#define LHDC_QUALITY_AUTO 13

#define LHDC_QUALITY_MAX_BITRATE 12

#define LHDC_QUALITY_HIGH5 12

#define LHDC_QUALITY_HIGH4 11

#define LHDC_QUALITY_HIGH3 10

#define LHDC_QUALITY_HIGH2 9

#define LHDC_QUALITY_HIGH1 8

#define LHDC_QUALITY_HIGH 7

#define LHDC_QUALITY_MID 6

#define LHDC_QUALITY_LOW 5

#define LHDC_QUALITY_LOW4 4

#define LHDC_QUALITY_LOW3 3

#define LHDC_QUALITY_LOW2 2

#define LHDC_QUALITY_LOW1 1

#define LHDC_QUALITY_LOW0 0

#define LHDC_MTU_MAX 8192

#define LHDC_MTU_MHDT_8DH5 2820

#define LHDC_MTU_MHDT_6DH5 2089

#define LHDC_MTU_MHDT_4DH5 1392

#define LHDC_MTU_3MBPS 1023

#define LHDC_MTU_2MBPS 660

#define LHDC_MTU_MIN 300

#define LHDC_VERSION_INVALID 2

#define LHDC_VERSION_1 1

#define LHDC_ENC_TYPE_INVALID 2

#define LHDC_ENC_TYPE_LHDC 1

#define LHDC_ENC_TYPE_UNKNOWN 0

#define LHDC_LOG_LEVEL_DEBUG 7

#define LHDC_LOG_LEVEL_INFO 6

#define LHDC_LOG_LEVEL_NOTICE 5

#define LHDC_LOG_LEVEL_WARNING 4

#define LHDC_LOG_LEVEL_ERROR 3

#define LHDC_LOG_LEVEL_CRIT 2

#define LHDC_LOG_LEVEL_ALERT 1

#define LHDC_LOG_LEVEL_EMERG 0

#define LHDC_FRET_BUF_NOT_ENOUGH -11

#define LHDC_FRET_ERROR -10

#define LHDC_FRET_AR_NOT_READY -9

#define LHDC_FRET_CODEC_NOT_READY -8

#define LHDC_FRET_INVALID_CODEC -7

#define LHDC_FRET_INVALID_HANDLE_AR -6

#define LHDC_FRET_INVALID_HANDLE_CBUF -5

#define LHDC_FRET_INVALID_HANDLE_ENC -4

#define LHDC_FRET_INVALID_HANDLE_PARA -3

#define LHDC_FRET_INVALID_HANDLE_CB -2

#define LHDC_FRET_INVALID_INPUT_PARAM -1

#define LHDC_FRET_SUCCESS 0

#define SEGMENT_RATE_480_LB 482

#define SEGMENT_RATE_480 481

#define SEGMENT_RATE_480_HR 480

void lhdcv5_enc_ffi_init();

STATUS_LHDC_BT lhdcv5_enc_ffi_get_handle(uint32_t version, HANDLE_LHDC_BT *handle);

STATUS_LHDC_BT lhdcv5_enc_ffi_free_handle(HANDLE_LHDC_BT handle);

STATUS_LHDC_BT lhdcv5_enc_ffi_init_encoder(HANDLE_LHDC_BT handle, uint32_t sampling_freq,
                                           uint32_t bits_per_sample, uint32_t bitrate_inx,
                                           uint32_t mtu, uint32_t interval);

/**
 * Returns current quality status
 */
STATUS_LHDC_BT lhdcv5_enc_ffi_get_quality_mode(HANDLE_LHDC_BT handle, uint32_t *quality_status);

/**
 * Returns current (last) bitrate
 */
STATUS_LHDC_BT lhdcv5_enc_ffi_get_last_bitrate(HANDLE_LHDC_BT handle, uint32_t *bitrate);

STATUS_LHDC_BT lhdcv5_enc_ffi_get_bitrate_index(HANDLE_LHDC_BT handle, uint32_t bitrate,
                                                uint32_t *bitrate_inx);

STATUS_LHDC_BT lhdcv5_enc_ffi_set_bitrate_index(HANDLE_LHDC_BT handle, uint32_t bitrate_inx,
                                                bool upd_qual_status);

STATUS_LHDC_BT lhdcv5_enc_ffi_set_max_bitrate(HANDLE_LHDC_BT handle, uint32_t max_bitrate_inx);

STATUS_LHDC_BT lhdcv5_enc_ffi_set_min_bitrate(HANDLE_LHDC_BT handle, uint32_t min_bitrate_inx);

STATUS_LHDC_BT lhdcv5_enc_ffi_get_block_size(HANDLE_LHDC_BT handle, uint32_t *samples_per_frame);

STATUS_LHDC_BT lhdcv5_enc_ffi_encode(HANDLE_LHDC_BT handle, const uint8_t *in_pcm,
                                     size_t in_pcm_len, uint8_t *out_buf, size_t out_buf_len,
                                     uint32_t *written_bytes, uint32_t *written_frames);
