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

#define LOG_TAG "lhdcv5BT_enc"

#include <log/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhdcv5BT.h"

#define CASE_RETURN_STR(const) \
  case const:                  \
    return #const;

/***************************************
 *  Auto Bit Rate Mechanism Parameters
 ***************************************/
typedef struct _lhdc_abr_para_s {
  uint32_t* abr_table;        // ptr to the actual ABR bitrate table
  uint32_t gABR_table_size;   // size of elements in the ABR bitrate table
  uint32_t gABR_table_index;  // current index in the ABR bitrate table
  uint32_t down_bitrate_count;
  uint32_t down_bitrate_sum;
  uint32_t up_bitrate_count;
  uint32_t up_bitrate_sum;
} lhdc_abr_para_t;

// store ABR parameters
lhdc_abr_para_t handle_abr;

#define LHDC_ABR_DEFAULT_BITRATE_INX (LHDC_QUALITY_LOW)

// ABR: policy parameters and bit rate adjustment tables
/*******************************************************************************/
#define ABR_MIN_STAGE_BITRATE 160  // min bitrate(kbps) used in auto_bitrate_adjust_table_...
#define ABR_MAX_STAGE_BITRATE \
  400  // max bitrate(kbps) (NOTE: must be smaller and equal to LHDC_ABR_MAX_BITRATE)
#define ABR_UP_RATE_TIME_CNT 3000        // ABR bitrate upgrade checking interval (by tick count)
#define ABR_DOWN_RATE_TIME_CNT 4         // ABR bitrate downgrade checking interval (by tick count)
#define ABR_UP_QUEUE_LENGTH_THRESHOLD 1  // The threshold of ABR bitrate upgrade condition
#define ABR_DOWN_QUEUE_LENGTH_THRESHOLD 0  // The threshold of ABR bitrate downgrade condition
#define ABR_DOWN_TARGET_STAGE 0  // The target bitrate stage of ABR table that ABR go when downgrade

static uint32_t auto_bitrate_adjust_table_lhdcv5_44k[] = {160, 192, 240,
                                                          320, 400, ABR_MAX_STAGE_BITRATE};
static uint32_t auto_bitrate_adjust_table_lhdcv5_48k[] = {160, 192, 256,
                                                          320, 400, ABR_MAX_STAGE_BITRATE};
static uint32_t auto_bitrate_adjust_table_lhdcv5_96k[] = {256, 320, 400,
                                                          400, 400, ABR_MAX_STAGE_BITRATE};
static uint32_t auto_bitrate_adjust_table_lhdcv5_192k[] = {256, 320, 400,
                                                           400, 400, ABR_MAX_STAGE_BITRATE};

#define LHDC_44K_BITRATE_ELEMENTS_SIZE \
  (sizeof(auto_bitrate_adjust_table_lhdcv5_44k) / sizeof(uint32_t))
#define LHDC_48K_BITRATE_ELEMENTS_SIZE \
  (sizeof(auto_bitrate_adjust_table_lhdcv5_48k) / sizeof(uint32_t))
#define LHDC_96K_BITRATE_ELEMENTS_SIZE \
  (sizeof(auto_bitrate_adjust_table_lhdcv5_96k) / sizeof(uint32_t))
#define LHDC_192K_BITRATE_ELEMENTS_SIZE \
  (sizeof(auto_bitrate_adjust_table_lhdcv5_192k) / sizeof(uint32_t))
/*******************************************************************************/

static const char* rate_to_string(uint32_t q) {
  switch (q) {
    CASE_RETURN_STR(LHDC_QUALITY_LOW0);
    CASE_RETURN_STR(LHDC_QUALITY_LOW1);
    CASE_RETURN_STR(LHDC_QUALITY_LOW2);
    CASE_RETURN_STR(LHDC_QUALITY_LOW3);
    CASE_RETURN_STR(LHDC_QUALITY_LOW4);
    CASE_RETURN_STR(LHDC_QUALITY_LOW);
    CASE_RETURN_STR(LHDC_QUALITY_MID);
    CASE_RETURN_STR(LHDC_QUALITY_HIGH);
    CASE_RETURN_STR(LHDC_QUALITY_HIGH1);
    CASE_RETURN_STR(LHDC_QUALITY_HIGH2);
    CASE_RETURN_STR(LHDC_QUALITY_HIGH3);
    CASE_RETURN_STR(LHDC_QUALITY_HIGH4);
    CASE_RETURN_STR(LHDC_QUALITY_HIGH5);
    CASE_RETURN_STR(LHDC_QUALITY_AUTO);
    CASE_RETURN_STR(LHDC_QUALITY_UNLIMIT);
    CASE_RETURN_STR(LHDC_QUALITY_CTRL_RESET_ABR);
    CASE_RETURN_STR(LHDC_QUALITY_CTRL_END);
    default:
      ALOGE("%s: Invalid quality(%d)", __func__, q);
      return "UNKNOWN_QUALITY";
  }
}

//----------------------------------------------------------------
// lhdcv5_enc_abr_adjust_bitrate ()
//
// Adjust bit rate automatically according to number of packets in queue for LHDC 5.0 encoding
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcBT_get_handle ()
//    handle: a pointer to ABR parameters
//    queueLen: number of packets in a2dp transmit queue
//  Return
//    LHDC_FRET_SUCCESS: succeed to adjust bit rate automatically
//    otherwise: fail to adjust bit rate automatically
//----------------------------------------------------------------
static int lhdcv5_enc_abr_adjust_bitrate(HANDLE_LHDC_BT handle, uint32_t queueLen) {
  int32_t func_ret = LHDC_FRET_SUCCESS;
  uint32_t last_bitrate = 0;
  uint32_t last_bitrate_inx = 0;
  uint32_t new_abr_bitrate_inx = 0;
  uint32_t new_bitrate = 0;
  uint32_t new_bitrate_inx = 0;

  bool upd_qual_status = false;
  uint32_t queueLength = 0;
  uint32_t queueSumTmp = 0;

  if (handle == NULL) {
    ALOGE("%s: handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  if (handle_abr.down_bitrate_count >= ABR_DOWN_RATE_TIME_CNT) {
    queueLength = handle_abr.down_bitrate_sum / handle_abr.down_bitrate_count;

    // clean ABR down statistics parameters
    handle_abr.down_bitrate_count = 0;
    handle_abr.down_bitrate_sum = 0;

    if (queueLength > ABR_DOWN_QUEUE_LENGTH_THRESHOLD) {
      // get last bitrate
      func_ret = lhdcv5_enc_ffi_get_last_bitrate(handle, &last_bitrate);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("[AUTO_BITRATE][ABR_ADJ](DN) lhdcv5_enc_ffi_get_last_bitrate error %d", func_ret);
        goto fail;
      }

      func_ret = lhdcv5_enc_ffi_get_bitrate_index(handle, last_bitrate, &last_bitrate_inx);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("[AUTO_BITRATE][ABR_ADJ](DN) lhdcv5_enc_ffi_get_bitrate_index error %d", func_ret);
        goto fail;
      }

      new_abr_bitrate_inx = ABR_DOWN_TARGET_STAGE;
      new_bitrate = handle_abr.abr_table[new_abr_bitrate_inx];

      func_ret = lhdcv5_enc_ffi_get_bitrate_index(handle, new_bitrate, &new_bitrate_inx);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("[AUTO_BITRATE][ABR_ADJ](DN) lhdcv5_enc_ffi_get_bitrate_index error %d", func_ret);
        goto fail;
      }

#if 0
      ALOGD ("[AUTO_BITRATE][ABR_ADJ](DN) last_bitrate:%u new_bitrate:%u", last_bitrate, new_bitrate);
      ALOGD ("[AUTO_BITRATE][ABR_ADJ](DN) last_bitrate_inx:%u new_bitrate_inx:%u",
          last_bitrate_inx, new_bitrate_inx);
      ALOGD ("[AUTO_BITRATE][ABR_ADJ](DN) gABR_table_index:%u new_abr_bitrate_inx:%u",
          handle_abr.gABR_table_index, new_abr_bitrate_inx);
#endif

      // check if need to downtier bitrate
      if ((new_bitrate_inx <= last_bitrate_inx) &&
          new_abr_bitrate_inx < handle_abr.gABR_table_index) {
        func_ret = lhdcv5_enc_ffi_set_bitrate_index(handle, new_bitrate_inx, upd_qual_status);
        if (func_ret != LHDC_FRET_SUCCESS) {
          ALOGE("[AUTO_BITRATE][ABR_ADJ](DN) lhdcv5_enc_ffi_set_bitrate_index error %d", func_ret);
          goto fail;
        }

        ALOGI("[AUTO_BITRATE][ABR_ADJ](DN) br_table[%u](%u) to br_table[%u][%u]",
              handle_abr.gABR_table_index, handle_abr.abr_table[handle_abr.gABR_table_index],
              new_abr_bitrate_inx, handle_abr.abr_table[new_abr_bitrate_inx]);

        // clean ABR up statistics parameters
        handle_abr.up_bitrate_count = 0;
        handle_abr.up_bitrate_sum = 0;
        handle_abr.gABR_table_index = new_abr_bitrate_inx;
      } else {
        ALOGD("[AUTO_BITRATE][ABR_ADJ](DN) bitrate not changed (%u)[%u]", last_bitrate,
              handle_abr.gABR_table_index);
      }
    }
  }

  if (handle_abr.up_bitrate_count >= ABR_UP_RATE_TIME_CNT) {
    queueSumTmp = handle_abr.up_bitrate_sum;

    // clean ABR up statistics parameters
    handle_abr.up_bitrate_count = 0;
    handle_abr.up_bitrate_sum = 0;

    if (queueSumTmp < ABR_UP_QUEUE_LENGTH_THRESHOLD) {
      // get last bitrate and index
      func_ret = lhdcv5_enc_ffi_get_last_bitrate(handle, &last_bitrate);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("[AUTO_BITRATE][ABR_ADJ](UP) lhdcv5_enc_ffi_get_last_bitrate error %d", func_ret);
        goto fail;
      }
      func_ret = lhdcv5_enc_ffi_get_bitrate_index(handle, last_bitrate, &last_bitrate_inx);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("[AUTO_BITRATE][ABR_ADJ](UP) lhdcv5_enc_ffi_get_bitrate_index error %d", func_ret);
        goto fail;
      }

      // configure new target bitrate and index
      if (handle_abr.gABR_table_index < (handle_abr.gABR_table_size - 1)) {
        new_abr_bitrate_inx = handle_abr.gABR_table_index + 1;
      }

      new_bitrate = handle_abr.abr_table[new_abr_bitrate_inx];

      func_ret = lhdcv5_enc_ffi_get_bitrate_index(handle, new_bitrate, &new_bitrate_inx);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("[AUTO_BITRATE][ABR_ADJ](UP) lhdcv5_enc_ffi_get_bitrate_index error %d", func_ret);
        goto fail;
      }

#if 0
      ALOGD ("[AUTO_BITRATE][ABR_ADJ](UP) last_bitrate:%u new_bitrate:%u", last_bitrate, new_bitrate);
      ALOGD ("[AUTO_BITRATE][ABR_ADJ](UP) last_bitrate_inx:%u new_bitrate_inx:%u",
          last_bitrate_inx, new_bitrate_inx);
      ALOGD ("[AUTO_BITRATE][ABR_ADJ](UP) gABR_table_index:%u new_abr_bitrate_inx:%u",
          handle_abr.gABR_table_index, new_abr_bitrate_inx);
#endif

      // check if need to uptier bitrate
      if ((new_bitrate_inx >= last_bitrate_inx) &&
          (new_abr_bitrate_inx > handle_abr.gABR_table_index)) {
        func_ret = lhdcv5_enc_ffi_set_bitrate_index(handle, new_bitrate_inx, upd_qual_status);
        if (func_ret != LHDC_FRET_SUCCESS) {
          ALOGE("[AUTO_BITRATE][ABR_ADJ](UP) lhdcv5_enc_ffi_set_bitrate_index error %d", func_ret);
          goto fail;
        }

        ALOGI("[AUTO_BITRATE][ABR_ADJ](UP) br_table[%u](%u) to br_table[%u][%u]",
              handle_abr.gABR_table_index, handle_abr.abr_table[handle_abr.gABR_table_index],
              new_abr_bitrate_inx, handle_abr.abr_table[new_abr_bitrate_inx]);

        // clean ABR down statistics parameters
        handle_abr.down_bitrate_count = 0;
        handle_abr.down_bitrate_sum = 0;
        handle_abr.gABR_table_index = new_abr_bitrate_inx;
      } else {
        ALOGD("[AUTO_BITRATE][ABR_ADJ](UP) bitrate not changed (%u)[%u]", last_bitrate,
              handle_abr.gABR_table_index);
      }
    }
  }

  if (queueLen > 0) {
    handle_abr.up_bitrate_sum += queueLen;
    handle_abr.down_bitrate_sum += queueLen;
  }

  handle_abr.up_bitrate_count++;
  handle_abr.down_bitrate_count++;

fail:
  return func_ret;
}

/*
 ******************************************************************
 LHDC library public API group
 ******************************************************************
 */

//----------------------------------------------------------------
// lhdcv5BT_free_handle ()
//
// Free all resources allocated
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcv5BT_get_handle ()
//  Return
//    LHDC_FRET_SUCCESS: Succeed to free all resources
//    otherwise: Fail to free all resources
//----------------------------------------------------------------
int32_t lhdcv5BT_free_handle(HANDLE_LHDC_BT handle) {
  int32_t func_ret = LHDC_FRET_ERROR;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  // reset resources
  func_ret = lhdcv5_enc_ffi_free_handle(handle);
  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("Failed to free handle");
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_get_handle ()
//
// Allocate resources required by LHDC 5.0 Encoder
//   Parameter
//    version: version defined in BT A2DP capability
//    handle: a pointer to the resource allocated
//   Return
//    LHDC_FRET_SUCCESS: Succeed to allocate resources
//    otherwise: Fail to allocate resources
//----------------------------------------------------------------
int32_t lhdcv5BT_get_handle(uint32_t version, HANDLE_LHDC_BT* handle) {
  int32_t func_ret = LHDC_FRET_SUCCESS;
  HANDLE_LHDC_BT hLhdcBT = NULL;

  if (version != LHDC_VERSION_1) {
    ALOGE("%s: Invalid version (%u)!", __func__, version);
    return LHDC_FRET_ERROR;
  }

  if (handle == NULL) {
    ALOGE("%s: Input parameter is NULL!", __func__);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  // register a logger callback function to lib
  lhdcv5_enc_ffi_init();

  func_ret = lhdcv5_enc_ffi_get_handle(version, &hLhdcBT);
  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("%s: Fail to get handle (%d)!", __func__, func_ret);
    free(hLhdcBT);
    return LHDC_FRET_ERROR;
  }

  *handle = hLhdcBT;

  if ((*handle) == NULL) {
    ALOGE("%s: Get handle NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_get_bitrate ()
//
// Get the bit rate used during LHDC 5.0 encoding
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcv5BT_get_handle ()
//    bitrate: a pointer to bit rate used during LHDC 5.0 encoding,
//         range [64000, 1000000]
//  Return
//    LHDC_FRET_SUCCESS: Succeed to allocate resources
//    otherwise: Fail to allocate resources
//----------------------------------------------------------------
int32_t lhdcv5BT_get_bitrate(HANDLE_LHDC_BT handle, uint32_t* bitrate) {
  int32_t func_ret = LHDC_FRET_SUCCESS;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  if (bitrate == NULL) {
    ALOGE("%s: Input parameter is NULL!", __func__);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  func_ret = lhdcv5_enc_ffi_get_last_bitrate(handle, bitrate);

  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("%s: Failed to get bit rate (%d)!", __func__, func_ret);
    return LHDC_FRET_ERROR;
  }

  ALOGI("%s: get current bitrate (%u)!", __func__, *bitrate);

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_set_bitrate ()
//
// Set the bit rate used during LHDC 5.0 encoding
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcBT_get_handle ()
//    bitrate_inx: an index of bit rate to set
//  Return
//    LHDC_FRET_SUCCESS: succeed to set the bit rate
//    Other: fail to set the bit rate
//----------------------------------------------------------------
int32_t lhdcv5BT_set_bitrate(HANDLE_LHDC_BT handle, uint32_t bitrate_inx) {
  bool upd_qual_status = false;
  uint32_t target_bitrate = 0;
  uint32_t target_bitrate_inx = 0;
  int32_t func_ret = LHDC_FRET_SUCCESS;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  // handle standard/control index
  switch (bitrate_inx) {
    case LHDC_QUALITY_CTRL_RESET_ABR: {
      // reset ABR table index
      handle_abr.gABR_table_index = LHDC_ABR_DEFAULT_BITRATE_INX;

      target_bitrate = handle_abr.abr_table[handle_abr.gABR_table_index];

      func_ret = lhdcv5_enc_ffi_get_bitrate_index(handle, target_bitrate, &target_bitrate_inx);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("[AUTO_BITRATE][ABR_ADJ](DN) lhdcv5_enc_ffi_get_bitrate_index error %d", func_ret);
        return LHDC_FRET_ERROR;
      }

      // change current actual bitrate only, not change quality index
      upd_qual_status = false;
      func_ret = lhdcv5_enc_ffi_set_bitrate_index(handle, target_bitrate_inx, upd_qual_status);
      if (func_ret != LHDC_FRET_SUCCESS) {
        ALOGE("%s: lhdcv5_enc_ffi_set_bitrate_index error (%d)!", __func__, func_ret);
        return LHDC_FRET_ERROR;
      }
      ALOGI("%s: [Reset BiTrAtE] (%s) ABR_table_index(%d)", __func__,
            rate_to_string(target_bitrate_inx), target_bitrate_inx);
    } break;

    // LOW0 ~ ABR
    case LHDC_QUALITY_AUTO:
    case LHDC_QUALITY_HIGH5:
    case LHDC_QUALITY_HIGH4:
    case LHDC_QUALITY_HIGH3:
    case LHDC_QUALITY_HIGH2:
    case LHDC_QUALITY_HIGH1:
    case LHDC_QUALITY_HIGH:
    case LHDC_QUALITY_MID:
    case LHDC_QUALITY_LOW:
    case LHDC_QUALITY_LOW4:
    case LHDC_QUALITY_LOW3:
    case LHDC_QUALITY_LOW2:
    case LHDC_QUALITY_LOW1:
    case LHDC_QUALITY_LOW0: {
      if (bitrate_inx == LHDC_QUALITY_AUTO) {
        // reset ABR table index
        handle_abr.gABR_table_index = LHDC_ABR_DEFAULT_BITRATE_INX;
      } else {
        // change current actual bitrate and also sync quality index
        upd_qual_status = true;
        func_ret = lhdcv5_enc_ffi_set_bitrate_index(handle, bitrate_inx, upd_qual_status);
        if (func_ret != LHDC_FRET_SUCCESS) {
          ALOGE("%s: lhdcv5_enc_ffi_set_bitrate_index error (%d)!", __func__, func_ret);
          return LHDC_FRET_ERROR;
        }
      }
    } break;

    default: {
      ALOGI("%s: Not supported index (%s)", __func__, rate_to_string(bitrate_inx));
      return LHDC_FRET_ERROR;
    } break;
  }

  ALOGI("%s: Update target bitrate (%s) bitrate_inx(%d)", __func__, rate_to_string(bitrate_inx),
        bitrate_inx);

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_set_max_bitrate ()
//
// Set the MAX. bit rate for LHDC 5.0 encoding
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcBT_get_handle ()
//    max_bitrate_inx: MAX. bit rate (index) for LHDC 5.0 encoding
//  Return
//    LHDC_FRET_SUCCESS: succeed to set the MAX. bit rate
//    Other: fail to set the MAX. bit rate
//----------------------------------------------------------------
int32_t lhdcv5BT_set_max_bitrate(HANDLE_LHDC_BT handle, uint32_t max_bitrate_inx) {
  int32_t func_ret = LHDC_FRET_SUCCESS;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  // max_bitrate_inx policy: LOW ~ MAX
  if ((max_bitrate_inx < LHDC_QUALITY_LOW) || (max_bitrate_inx > LHDC_QUALITY_MAX_BITRATE)) {
    ALOGE("%s: Invalid max bit rate index (%u)!", __func__, max_bitrate_inx);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  func_ret = lhdcv5_enc_ffi_set_max_bitrate(handle, max_bitrate_inx);
  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("%s: failed to set max. bit rate index (%u), (%d)!", __func__, max_bitrate_inx, func_ret);
    return LHDC_FRET_ERROR;
  }

  ALOGI("%s: Update Max target bitrate(%s)", __func__, rate_to_string(max_bitrate_inx));

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_set_min_bitrate ()
//
// Set the MIN. bit rate for LHDC 5.0 encoding
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcBT_get_handle ()
//    min_bitrate_inx: MIN. bit rate (index) for LHDC 5.0 encoding
//  Return
//    LHDC_FRET_SUCCESS: succeed to set the MIN. bit rate
//    Other: fail to set the MIN. bit rate
//----------------------------------------------------------------

int32_t lhdcv5BT_set_min_bitrate(HANDLE_LHDC_BT handle, uint32_t min_bitrate_inx) {
  int32_t func_ret = LHDC_FRET_SUCCESS;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  // min policy: (LOW0 ~ LOW)
  if ((min_bitrate_inx < LHDC_QUALITY_LOW0) || (min_bitrate_inx > LHDC_QUALITY_LOW)) {
    ALOGE("%s: Invalid min bit rate index (%u)!", __func__, min_bitrate_inx);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  func_ret = lhdcv5_enc_ffi_set_min_bitrate(handle, min_bitrate_inx);

  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("%s: failed to set min. bit rate (%d)!", __func__, func_ret);
    return LHDC_FRET_ERROR;
  }

  ALOGI("%s: Update Min target bitrate(%s)", __func__, rate_to_string(min_bitrate_inx));

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_adjust_bitrate () - ABR
//
// Adjust bit rate automatically according to number of packets in queue for LHDC 5.0 encoding
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcBT_get_handle ()
//    queue_len: number of packets in queue
//  Return
//    LHDC_FRET_SUCCESS: succeed to adjust bit rate automatically
//    Other: fail to adjust bit rate automatically
//----------------------------------------------------------------

int32_t lhdcv5BT_adjust_bitrate(HANDLE_LHDC_BT handle, uint32_t queueLen) {
  int32_t func_ret = LHDC_FRET_ERROR;
  uint32_t quality_status = 0;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  // get current quality status (lhdc bitrate operation mode)
  func_ret = lhdcv5_enc_ffi_get_quality_mode(handle, &quality_status);
  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("fail to get quality status! %d", func_ret);
    return LHDC_FRET_ERROR;
  }

  if (quality_status != LHDC_QUALITY_AUTO) {
    ALOGE("error! quality_status is not auto bit rate mode!");
    return LHDC_FRET_ERROR;
  }

  // ABR mode (lossy mode) only
  func_ret = lhdcv5_enc_abr_adjust_bitrate(handle, queueLen);
  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("%s: Failed to adjust auto bit rate (%d)!", __func__, func_ret);
    return LHDC_FRET_ERROR;
  }

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_init_encoder ()
//
// Initialize LHDC 5.0 encoder
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcBT_get_handle ()
//    sampling_freq: sample frequency
//    bit_per_sample: bits per sample
//    bitrate_inx: bit rate index
//    mtu: BT A2DP MTU
//    interval: interval: period of time triggering LHDC 5.0 encoding in ms
//  Return
//    LHDC_FRET_SUCCESS: succeed to initialize
//    Other: fail to initialize.
//----------------------------------------------------------------
int32_t lhdcv5BT_init_encoder(HANDLE_LHDC_BT handle, uint32_t sampling_freq,
                              uint32_t bits_per_sample, uint32_t bitrate_inx, uint32_t mtu,
                              uint32_t interval, [[maybe_unused]] uint32_t reserved) {
  uint32_t abr_table_size = 0;
  int32_t func_ret = LHDC_FRET_SUCCESS;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  if ((sampling_freq != LHDC_SR_44100HZ) && (sampling_freq != LHDC_SR_48000HZ) &&
      (sampling_freq != LHDC_SR_96000HZ) && (sampling_freq != LHDC_SR_192000HZ)) {
    ALOGE("%s: Invalid sampling frequency (%u)!", __func__, sampling_freq);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  if ((bits_per_sample != LHDCBT_SMPL_FMT_S16) && (bits_per_sample != LHDCBT_SMPL_FMT_S24)) {
    ALOGE("%s: Invalid bits per sample (%u)!", __func__, bits_per_sample);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  // LOW0 ~ ABR
  if ((bitrate_inx < LHDC_QUALITY_LOW0) || (bitrate_inx > LHDC_QUALITY_AUTO)) {
    ALOGE("%s: Invalid bit rate (index) (%d)!", __func__, bitrate_inx);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  func_ret = lhdcv5_enc_ffi_init_encoder(handle, sampling_freq, bits_per_sample, bitrate_inx, mtu,
                                         interval);
  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("%s: Failed to init LHDC 5.0 encoder (%d)!", __func__, func_ret);
    return LHDC_FRET_ERROR;
  }

  // configure ABR, VBR control parameters
  memset(&handle_abr, 0, sizeof(lhdc_abr_para_t));

  if (bitrate_inx == LHDC_QUALITY_AUTO) {
    if (sampling_freq == LHDC_SR_44100HZ) {
      handle_abr.abr_table = &(auto_bitrate_adjust_table_lhdcv5_44k[0]);
      handle_abr.gABR_table_size = LHDC_44K_BITRATE_ELEMENTS_SIZE;
      handle_abr.gABR_table_index = LHDC_44K_BITRATE_ELEMENTS_SIZE - 1;
    } else if (sampling_freq == LHDC_SR_48000HZ) {
      handle_abr.abr_table = &(auto_bitrate_adjust_table_lhdcv5_48k[0]);
      handle_abr.gABR_table_size = LHDC_48K_BITRATE_ELEMENTS_SIZE;
      handle_abr.gABR_table_index = LHDC_48K_BITRATE_ELEMENTS_SIZE - 1;
    } else if (sampling_freq == LHDC_SR_96000HZ) {
      handle_abr.abr_table = &(auto_bitrate_adjust_table_lhdcv5_96k[0]);
      handle_abr.gABR_table_size = LHDC_96K_BITRATE_ELEMENTS_SIZE;
      handle_abr.gABR_table_index = LHDC_96K_BITRATE_ELEMENTS_SIZE - 1;
    } else if (sampling_freq == LHDC_SR_192000HZ) {
      handle_abr.abr_table = &(auto_bitrate_adjust_table_lhdcv5_192k[0]);
      handle_abr.gABR_table_size = LHDC_192K_BITRATE_ELEMENTS_SIZE;
      handle_abr.gABR_table_index = LHDC_192K_BITRATE_ELEMENTS_SIZE - 1;
    } else {
      // should not be here
    }

    handle_abr.down_bitrate_count = 0;
    handle_abr.down_bitrate_sum = 0;
    handle_abr.up_bitrate_count = 0;
    handle_abr.up_bitrate_sum = 0;
  }

  ALOGI("%s: success!", __func__);

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_get_block_Size ()
//
// Get number of samples per block for LHDC 5.0 encoder
//  Parameter
//    handle: a pointer to the resource allocated and is returned by function
//    lhdcv5Bt_get_handle ()     samples_per_frame: number of samples per block returned
//
//  Return
//    LHDC_FRET_SUCCESS: succeed to get number of samples per block
//    Other: fail to get number of samples per block.
//----------------------------------------------------------------
int32_t lhdcv5BT_get_block_Size(HANDLE_LHDC_BT handle, uint32_t* samples_per_frame) {
  int32_t func_ret = LHDC_FRET_SUCCESS;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  if (samples_per_frame == NULL) {
    ALOGE("%s: Input parameter is NULL!", __func__);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  func_ret = lhdcv5_enc_ffi_get_block_size(handle, samples_per_frame);

  if ((func_ret != LHDC_FRET_SUCCESS) || ((*samples_per_frame) <= 0)) {
    ALOGE("%s: Failed to get block size (%d) (%d)!", __func__, func_ret, *samples_per_frame);
    return LHDC_FRET_ERROR;
  }

  return LHDC_FRET_SUCCESS;
}

//----------------------------------------------------------------
// lhdcv5BT_encode ()
//
// Encode pcm samples by LHDC 5.0
//  Parameter
//    handle: a pointer to the resource allocated and is returned
//        by function lhdcBT_get_handle ()
//    p_in_pcm: a pointer to a buffer contains PCM samples for encoding
//    p_out_buf: a pointer to a buffer to put encoded stream
//    out_buf_bytes: output buffer's size (in byte)
//    p_out_bytes: a pointer to number of bytes of encoded stream in buffer
//    p_out_frames: a pointer to number of frames of encoded stream in buffer
//  Return
//    LHDC_FRET_SUCCESS: succeed to encode pcm samples
//    Other: fail to encode pcm samples
//----------------------------------------------------------------
int32_t lhdcv5BT_encode(HANDLE_LHDC_BT handle, void* p_in_pcm, uint32_t pcm_bytes,
                        uint8_t* p_out_buf, uint32_t out_buf_bytes, uint32_t* p_out_bytes,
                        uint32_t* p_out_frames) {
  int32_t func_ret = LHDC_FRET_SUCCESS;

  if (handle == NULL) {
    ALOGE("%s: Handle is NULL!", __func__);
    return LHDC_FRET_INVALID_HANDLE_CB;
  }

  if ((p_in_pcm == NULL) || (p_out_buf == NULL) || (p_out_bytes == NULL) ||
      (p_out_frames == NULL)) {
    ALOGE("%s: input parameter is NULL!", __func__);
    return LHDC_FRET_INVALID_INPUT_PARAM;
  }

  func_ret = lhdcv5_enc_ffi_encode(handle, (uint8_t*)p_in_pcm, (uintptr_t)pcm_bytes, p_out_buf,
                                   (uintptr_t)out_buf_bytes, p_out_bytes, p_out_frames);

  if (func_ret != LHDC_FRET_SUCCESS) {
    ALOGE("%s: Failed to encode pcm samples (%d)!", __func__, func_ret);
    return LHDC_FRET_ERROR;
  }

  return LHDC_FRET_SUCCESS;
}
