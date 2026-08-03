// Copyright (C) 2025, The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use crate::arith;
use crate::bits::output::BitOutputter;
use crate::enc::context::Context;
use crate::lhdc_enc::lhdc_enc_freq_process::*;
use crate::lhdc_enc::lhdc_enc_header::{enc_process_header, Header, SegmentSettings};
use crate::lhdc_enc::lhdc_enc_workspace::*;
use crate::math::{div, div_round, log_2};
use zerocopy::{FromBytes, IntoBytes};

mod tables;
use tables::*;

use thiserror::Error;
#[derive(Debug, Error)]
pub enum Error {
    #[error(transparent)]
    Arith(#[from] crate::arith::Error),
    #[error(transparent)]
    BitOutputter(#[from] crate::bits::output::Error),
    #[error("Placeholder haven't grokked this file yet")]
    TargetRange,
    #[error("Input parameters out of range")]
    BadParams,
    // TODO(b/454096420), ideally this shouldn't exist, but we have LHDC_ENC_ERROR to convert from
    #[error("Generic Encoder Error")]
    Encoder,
}

pub type Result<T> = core::result::Result<T, Error>;

/// Computes a moving average for the input vector
/// The averaging window extends one below and 2 above the index being calculated for.
///
/// Edge cases:
/// * The leftmost output just uses a window size of 1 (e.g. just itself)
/// * The second-to-last output uses a window size of 3 (left, itself, right).
/// * The last output uses a window size of 1, pointed at the value one to the left.
fn moving_average(input: &[i32], output: &mut [i32]) {
    const WINDOW: usize = 4;
    output[0] = input[0];
    let mut total: i32 = input[0..WINDOW].iter().sum();
    output[1] = div_round(total, 4);
    let mut i = 2;
    while i < (input.len() - 2) {
        total += input[i + 2] - input[i - 2];
        output[i] = div_round(total, 4);
        i += 1;
    }
    total -= input[i - 2];
    output[i] = div::<3>(total);
    i += 1;
    output[i] = input[i - 1];
}

static JUMP_TABLE: [i32; 64] = [
    6, 13, 19, 26, 32, 38, 45, 51, 58, 64, 77, 90, 102, 115, 128, 141, 154, 166, 186, 205, 224,
    243, 262, 282, 301, 326, 352, 378, 403, 429, 461, 493, 525, 557, 595, 634, 672, 717, 762, 813,
    870, 934, 1005, 1082, 1165, 1254, 1350, 1453, 1562, 1677, 1798, 1926, 2061, 2202, 2349, 2502,
    2662, 2829, 3002, 3187, 3386, 3603, 3840, 4096,
];

fn lhdc_enc_symbol_generation_step_1(
    idx: &mut i32,
    queue1: &[u32],
    mut codeing_step_0: i32,
    queue2: &mut [u8],
) {
    let mut data2_idx: usize = 0;
    let size = queue1.len();
    codeing_step_0 = if codeing_step_0 as usize > size { size as i32 } else { codeing_step_0 };
    let idx_max = size - codeing_step_0 as usize;
    for i in 0..size {
        if queue1[i] > 1 && data2_idx <= idx_max {
            data2_idx = i;
            queue2[i] = (1 << 2) - 1 - 1;
        } else {
            queue2[i] = (queue1[i] & 1) as u8;
        }
    }
    *idx = data2_idx as i32;
}

fn lhdc_enc_symbol_generation_step_2(
    idx: i32,
    queue1: &[u32],
    queue2: &mut [u8],
    lossy_symbol_size_top: i32,
) -> i32 {
    let mut tmp: u32 = 0;
    let data2_idx: i32 = idx;
    let mut size2: usize = queue1.len();
    let mut current_block_44: u64;
    for (i, elem) in queue1.iter().copied().enumerate() {
        if i > data2_idx as usize {
            tmp = elem >> 1;
            current_block_44 = 7815301370352969686;
        } else if elem <= 1 {
            current_block_44 = 6239978542346980191;
        } else {
            tmp = elem - 2;
            current_block_44 = 7815301370352969686;
        }
        if current_block_44 == 7815301370352969686 {
            let mut stop = 0;
            if tmp != 0 {
                loop {
                    if size2 as i32 >= lossy_symbol_size_top - 3 {
                        return -1;
                    }
                    if tmp < (1 << 2) {
                        break;
                    }
                    stop += 1;
                    queue2[size2] = (1 << 2) - 1 - 1;
                    size2 += 1;
                    queue2[size2] = ((tmp >> 1) & 1) as u8;
                    size2 += 1;
                    queue2[size2] = (tmp & 1) as u8;
                    size2 += 1;
                    tmp >>= 2;
                }
                tmp = if stop != 0 { tmp - 1 } else { tmp };
                if (size2 + tmp as usize) >= lossy_symbol_size_top as usize {
                    return -1;
                }
                if tmp != 0 {
                    match tmp {
                        1 => {
                            queue2[size2] = 1;
                            size2 += 1;
                        }
                        2 => {
                            queue2[size2] = 1;
                            size2 += 1;
                            queue2[size2] = 1;
                            size2 += 1;
                        }
                        _ => {
                            queue2[size2] = 1;
                            size2 += 1;
                            queue2[size2] = 1;
                            size2 += 1;
                            queue2[size2] = 1;
                            size2 += 1;
                        }
                    }
                }
            }
            if size2 >= lossy_symbol_size_top as usize {
                return -1;
            }
            if tmp <= 2 {
                queue2[size2] = 0;
                size2 += 1;
            }
        }
    }
    size2 as i32
}
fn init_bn(enc_text: &mut fdata_all_buffer_struct) -> i32 {
    let mut init_bn = enc_text.fdata_enc_buffer.level.fdata_q_quant_bit_num[1];
    init_bn = if enc_text.fdata_quant_rem_data_size == 0 {
        0
    } else if enc_text.fdata_quant_rem_data_size_top < init_bn {
        enc_text.fdata_quant_rem_data_size_top as _
    } else {
        init_bn
    };
    init_bn
}

fn coef_calc(level: &mut fdata_buffer_struct, init_bn: i32, enc_pixel_size_byte: usize) {
    let bits_num = &mut level.fdata_q_quant_bit_num;
    let coef7 = <[i32]>::mut_from_bytes(level.fdata_q_quant_2_symbol.as_mut_bytes()).unwrap();
    let coef3 = &mut level.fdata_q_quant_rem;
    coef3[0] = init_bn;
    coef7[0] = init_bn;
    bits_num[0] = init_bn;
    let mut diff3 = 0;
    let mut diff7 = 0;
    if enc_pixel_size_byte < 200 {
        for i in 1..level.valid as usize {
            coef3[i] = (coef3[i - 1] * 3 + bits_num[i] * 5) >> 3;
            let diff_3_bits = coef3[i] - bits_num[i + 1];
            diff3 += diff_3_bits ^ (diff_3_bits >> 31);
            coef7[i] = (coef7[i - 1] * 7 + bits_num[i]) >> 3;
            let diff_7_bits = coef7[i] - bits_num[i + 1];
            diff7 += diff_7_bits ^ (diff_7_bits >> 31);
        }
        level.symbol_flag = (diff7 <= diff3) as core::ffi::c_int;
    } else {
        for i in 1..level.valid as usize {
            coef7[i] = (coef7[i - 1] * 7 + bits_num[i]) >> 3;
        }
        level.symbol_flag = 1;
    }
}
fn lhdc_enc_symbol_generation(
    queue1: &[u32],
    codeing_step_0: i32,
    queue2: &mut [u8],
    lossy_symbol_size_top: i32,
) -> i32 {
    let mut data2_idx: i32 = 0;
    let size = queue1.len();
    lhdc_enc_symbol_generation_step_1(&mut data2_idx, queue1, codeing_step_0, queue2);
    data2_idx = if data2_idx < size as i32 - codeing_step_0 { size as i32 } else { data2_idx };
    lhdc_enc_symbol_generation_step_2(data2_idx, queue1, queue2, lossy_symbol_size_top)
}
fn lhdc_enc_symbol_calc(enc_text: &mut fdata_all_buffer_struct) -> i32 {
    let umem8 = enc_text.fdata_enc_buffer.level.fdata_q_quant_bit_num.as_bytes();
    let size1 = enc_text.fdata_enc_buffer.level.valid;
    let size2 = enc_text.fdata_enc_buffer.level.fdata_q_size;

    let umem8 = &umem8[..size2 as usize];
    let mut total = arith::esti(&umem8[..size1 as usize], &enc_text.init_fq_tbl_01);
    total += arith::esti(&umem8[size1 as usize..], &enc_text.init_fq_tbl_02);
    total = (enc_text.esti_diff_value * total) >> 16;
    total
}
fn lhdc_enc_nbyte_out(
    enc_text: &mut fdata_all_buffer_struct,
    enc_arith_base: &mut arith::EncoderBase,
    bit_outputter: &mut BitOutputter,
) -> Result<()> {
    // This is `lossy_data_not_change`
    let lossy_uc = enc_text.fdata_enc_buffer.level.fdata_q_quant_bit_num.as_bytes();
    let umem8: &mut [u8] =
        &mut enc_text.fdata_enc_buffer.level.fdata_q_quant_2_symbol.as_mut_bytes()
            [0..enc_text.pixel_num_top as usize * 4];
    let mut enc_arith = arith::Encoder::new(enc_arith_base, umem8);
    enc_arith.program(&enc_text.init_fq_tbl_01, enc_text.segment_setting.arith_init_0);
    for uc in lossy_uc.iter().take(enc_text.fdata_enc_buffer.level.valid as usize) {
        enc_arith.symbol_3(*uc as u32)?;
    }
    enc_arith.program(&enc_text.init_fq_tbl_02, enc_text.segment_setting.arith_init_1);
    let size = (enc_text.fdata_enc_buffer.level.fdata_q_size
        - enc_text.fdata_enc_buffer.level.valid) as usize;
    for i in 0..size {
        enc_arith.symbol_3(lossy_uc[enc_text.fdata_enc_buffer.level.valid as usize + i] as u32)?;
    }
    let i = enc_arith.clear(bit_outputter)?;
    enc_text.fdata_enc_buffer.level.fdata_q_true_len = i << 3_i32;
    Ok(())
}
fn lhdc_enc_nbyte_esti(enc_text: &mut fdata_all_buffer_struct) -> i32 {
    let bits_num = &mut enc_text.fdata_enc_buffer.level.fdata_q_quant_bit_num;
    let real_data: &[u32] =
        &<[u32]>::ref_from_bytes(enc_text.fdata_enc_buffer.level.fdata_q_quant.as_bytes()).unwrap()
            [enc_text.pixel_num as usize - enc_text.fdata_enc_buffer.level.valid as usize..];
    for i in 0..enc_text.fdata_enc_buffer.level.valid as usize {
        bits_num[i + 1] = (log_2(real_data[i]) as i32) << 7_i32;
    }
    let ibn = init_bn(enc_text);
    let level = &mut enc_text.fdata_enc_buffer.level;
    coef_calc(level, ibn, enc_text.enc_pixel_size_byte as _);
    let real_data: &[u32] = &<[u32]>::ref_from_bytes(level.fdata_q_quant.as_bytes()).unwrap()
        [enc_text.pixel_num as usize - level.valid as usize..];

    let mut total: u32 = 0;
    if level.symbol_flag == 1 {
        // coef7, doesn't alias rb, but does alias queue1
        let coef = <[i32]>::mut_from_bytes(level.fdata_q_quant_2_symbol.as_mut_bytes()).unwrap();
        let rb = &mut level.fdata_q_quant_rem;
        for i in 0..level.valid as usize {
            let reserved_bits =
                (coef[i] + (1_i32 << 7 as core::ffi::c_int >> 1 as core::ffi::c_int)) >> 7_i32;
            // TODO(b/454096420) is this cast an issue?
            coef[i] = (real_data[i] >> reserved_bits) as _;
            rb[i] = reserved_bits;
            total = total.wrapping_add(reserved_bits as u32);
        }
    } else {
        // coef3, it aliases rb, but doesn't alias queue1
        let coef = &mut level.fdata_q_quant_rem;
        for i in 0..level.valid as usize {
            let queue_1 = &mut level.fdata_q_quant_2_symbol;
            let reserved_bits =
                (coef[i] + (1_i32 << 7 as core::ffi::c_int >> 1 as core::ffi::c_int)) >> 7_i32;
            queue_1[i] = real_data[i] >> reserved_bits;
            coef[i] = reserved_bits;
            total = total.wrapping_add(reserved_bits as u32);
        }
    }
    let real_queue_1 = &mut level.fdata_q_quant_2_symbol;
    enc_text.fdata_enc_buffer.level.fdata_q_size = lhdc_enc_symbol_generation(
        &real_queue_1[0..level.valid as usize],
        enc_text.segment_setting.codeing_step_0,
        // TODO(b/454096420) lossy_data_not_change
        level.fdata_q_quant_bit_num.as_mut_bytes(),
        enc_text.lossy_symbol_size_top,
    );
    if enc_text.fdata_enc_buffer.level.fdata_q_size < 0_i32 {
        return enc_text.target_range + 1_i32;
    }
    enc_text.fdata_enc_buffer.level.fdata_q_esti_len = lhdc_enc_symbol_calc(enc_text);
    total = total.wrapping_add(enc_text.fdata_enc_buffer.level.fdata_q_len as u32);
    total = total.wrapping_add(enc_text.fdata_enc_buffer.level.fdata_q_esti_len as u32);
    total as i32
}
fn index_calc(idx: i32, max_idx: i32, compare: i32, first_idx: i32) -> i32 {
    let mut index: i32 = idx;
    while index <= max_idx && JUMP_TABLE[index as usize] < compare {
        index += 1;
    }
    index = if index > first_idx { index - 1_i32 } else { index };
    index
}
fn lhdc_enc_lossy_jump_adust_process(
    in_0: &[i32],
    out: &mut [i32],
    size: i32,
    bindata: &mut [u8],
    first_idx: &mut i32,
    max_idx: i32,
) {
    let mut compare = in_0[1];
    compare = if compare < 0_i32 { -compare } else { compare };
    let mut index = *first_idx;
    index = index_calc(index, max_idx, compare, *first_idx);
    *first_idx = index;
    let mut est_control = 1_i32;
    out[0] = in_0[0];
    let mut estimate = out[0];
    for i in 0..(size as usize - 1) {
        compare = in_0[i + 1] - estimate;
        bindata[i] =
            (if compare < 0_i32 { 1 as core::ffi::c_int } else { 0 as core::ffi::c_int }) as core::ffi::c_uchar;
        if i != 0 {
            let tmp = bindata[i] as i32 - bindata[i - 1] as i32;
            match tmp {
                0 => {
                    est_control += 1_i32;
                    index += est_control;
                    index = if index >= 64_i32 { 64_i32 - 1 as core::ffi::c_int } else { index };
                }
                _ => {
                    index = (index * 3_i32 + 2 as core::ffi::c_int) >> 2 as core::ffi::c_int;
                    index = if index < 0_i32 { 0 as core::ffi::c_int } else { index };
                    est_control = 1_i32;
                }
            }
        }
        let sign_step =
            if bindata[i] != 0 { -JUMP_TABLE[index as usize] } else { JUMP_TABLE[index as usize] };
        estimate += sign_step;
        out[i + 1] = estimate;
    }
}
pub fn lhdc_enc_freq_shift(
    segment: &mut SegmentSettings,
    cutoff: i32,
    data: &mut [i32],
    data_sft: &mut [i32],
    jump: &mut [u8],
    first_idx: &mut i32,
    logarithm_by_2_tbl_0: &mut [u32],
) -> i32 {
    let mut se: [i32; 32] = [0; 32];
    let mut dirty_bottom = 0_i32;
    let se = &mut se[..segment.segment_num];
    for (s, elem) in se.iter_mut().enumerate() {
        *elem = 0_i32;
        for datum in &data[segment.segment_offset[s]..segment.segment_offset[s + 1]] {
            *elem = (*elem as core::ffi::c_longlong + ((*datum as i64 * *datum as i64) >> 31)) as i32;
        }
        *elem = ((*elem as core::ffi::c_longlong * segment.segment_scale[s] as core::ffi::c_longlong)
            >> segment.segment_scale_jump) as i32;
        dirty_bottom += *elem;
    }
    data_sft[segment.segment_num - 2] = (((644245094_i32 as core::ffi::c_longlong
        * se[segment.segment_num - 1] as core::ffi::c_longlong)
        >> 31)
        + ((858993459_i32 as core::ffi::c_longlong * se[segment.segment_num - 2] as core::ffi::c_longlong)
            >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[segment.segment_num - 3] as core::ffi::c_longlong)
            >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[segment.segment_num - 4] as core::ffi::c_longlong)
            >> 31)) as core::ffi::c_int;
    data_sft[segment.segment_num - 1] = (((1503238553_i32 as core::ffi::c_longlong
        * se[segment.segment_num - 1] as core::ffi::c_longlong)
        >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[segment.segment_num - 2] as core::ffi::c_longlong)
            >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[segment.segment_num - 3] as core::ffi::c_longlong)
            >> 31)) as core::ffi::c_int;
    data_sft[0] = (((1503238553_i32 as core::ffi::c_longlong * se[0] as core::ffi::c_longlong) >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[1] as core::ffi::c_longlong) >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[2] as core::ffi::c_longlong) >> 31))
        as core::ffi::c_int;
    data_sft[1] = (((644245094_i32 as core::ffi::c_longlong * se[0] as core::ffi::c_longlong) >> 31)
        + ((858993459_i32 as core::ffi::c_longlong * se[1] as core::ffi::c_longlong) >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[2] as core::ffi::c_longlong) >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[3] as core::ffi::c_longlong) >> 31))
        as core::ffi::c_int;
    for i in 2..(segment.segment_num - 2) {
        data_sft[i] = (((214748364_i32 as core::ffi::c_longlong
            * (se[i - 2] as core::ffi::c_longlong + se[i + 2] as core::ffi::c_longlong))
            >> 31)
            + ((858993459_i32 as core::ffi::c_longlong * se[i] as core::ffi::c_longlong) >> 31)
            + ((429496729_i32 as core::ffi::c_longlong
                * (se[i - 1] as core::ffi::c_longlong + se[i + 1] as core::ffi::c_longlong))
                >> 31)) as core::ffi::c_int;
    }
    dirty_bottom = ((dirty_bottom as core::ffi::c_longlong
        * segment.segment_num_inv as core::ffi::c_longlong)
        >> 31) as core::ffi::c_int;
    dirty_bottom =
        ((dirty_bottom as core::ffi::c_longlong * 214748_i32 as core::ffi::c_longlong) >> 31) as core::ffi::c_int;
    dirty_bottom = if dirty_bottom < 1_i32 { 1 as core::ffi::c_int } else { dirty_bottom };

    let mut val = dirty_bottom as u32;
    let mut tmp;

    if val < 64 {
        tmp = logarithm_by_2_tbl_0[val as usize] as _;
    } else {
        tmp = log_2(val) as _;
        val >>= tmp - 6;
        tmp = (logarithm_by_2_tbl_0[val as usize]).wrapping_add(((tmp - 1) << 8) as u32) as i32;
    }

    let log_dirty_bottom =
        (tmp + 31_i32 * ((1 as core::ffi::c_int) << 8 as core::ffi::c_int)) >> 1 as core::ffi::c_int;
    let mut avg = 0_i32;
    for s in 0..segment.segment_num {
        val = data_sft[s] as u32;

        if val < 64 {
            tmp = logarithm_by_2_tbl_0[val as usize] as i32;
        } else {
            tmp = log_2(val) as _;
            val >>= tmp - 6_i32;
            tmp = logarithm_by_2_tbl_0[val as usize]
                .wrapping_add(((tmp - 1) << 8 as core::ffi::c_int) as u32) as i32;
        }

        se[s] = if data_sft[s] < dirty_bottom {
            log_dirty_bottom
        } else {
            (tmp + 31_i32 * ((1 as core::ffi::c_int) << 8 as core::ffi::c_int)) >> 1 as core::ffi::c_int
        };
        avg += se[s];
    }
    avg = ((avg as core::ffi::c_longlong * segment.segment_num_inv as core::ffi::c_longlong) >> 31)
        as core::ffi::c_int;
    data_sft[segment.segment_num - 2] = (((644245094_i32 as core::ffi::c_longlong
        * se[segment.segment_num - 1] as core::ffi::c_longlong)
        >> 31)
        + ((858993459_i32 as core::ffi::c_longlong * se[segment.segment_num - 2] as core::ffi::c_longlong)
            >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[segment.segment_num - 3] as core::ffi::c_longlong)
            >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[segment.segment_num - 4] as core::ffi::c_longlong)
            >> 31)) as core::ffi::c_int;
    data_sft[segment.segment_num - 1] = (((1503238553_i32 as core::ffi::c_longlong
        * se[segment.segment_num - 1] as core::ffi::c_longlong)
        >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[segment.segment_num - 2] as core::ffi::c_longlong)
            >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[segment.segment_num - 3] as core::ffi::c_longlong)
            >> 31)) as core::ffi::c_int;
    data_sft[0] = (((1503238553 as core::ffi::c_int as core::ffi::c_longlong * se[0] as core::ffi::c_longlong)
        >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[1] as core::ffi::c_longlong) >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[2] as core::ffi::c_longlong) >> 31))
        as core::ffi::c_int;
    data_sft[1] = (((644245094 as core::ffi::c_int as core::ffi::c_longlong * se[0] as core::ffi::c_longlong)
        >> 31)
        + ((858993459_i32 as core::ffi::c_longlong * se[1] as core::ffi::c_longlong) >> 31)
        + ((429496729_i32 as core::ffi::c_longlong * se[2] as core::ffi::c_longlong) >> 31)
        + ((214748364_i32 as core::ffi::c_longlong * se[3] as core::ffi::c_longlong) >> 31))
        as core::ffi::c_int;
    for i in 2..(segment.segment_num - 2) {
        data_sft[i] = (((214748364_i32 as core::ffi::c_longlong
            * (se[i - 2] as core::ffi::c_longlong + se[i + 2] as core::ffi::c_longlong))
            >> 31)
            + ((858993459_i32 as core::ffi::c_longlong * se[i] as core::ffi::c_longlong) >> 31)
            + ((429496729_i32 as core::ffi::c_longlong
                * (se[i - 1] as core::ffi::c_longlong + se[i + 1] as core::ffi::c_longlong))
                >> 31)) as core::ffi::c_int;
    }
    if 4250_i32 > avg && (480 as core::ffi::c_int) < cutoff {
        jump[0..(segment.segment_num - 1)].fill(0);
        *first_idx = 8_i32 + ((1 as core::ffi::c_int) << 4 as core::ffi::c_int) - 2 as core::ffi::c_int + 1_i32;
        return avg;
    }
    data_sft[0] = (((data_sft[0] as core::ffi::c_longlong - avg as core::ffi::c_longlong)
        * segment.segment_scale_level as core::ffi::c_longlong)
        >> 31) as core::ffi::c_int;
    for s in 1..segment.segment_num {
        data_sft[s] = (((data_sft[s] as core::ffi::c_longlong - avg as core::ffi::c_longlong)
            * segment.segment_scale_level as core::ffi::c_longlong)
            >> 31) as core::ffi::c_int;
        data_sft[s] -= data_sft[0];
    }
    data_sft[0] = 0;
    *first_idx = 8;
    lhdc_enc_lossy_jump_adust_process(
        data_sft,
        se,
        segment.segment_num as _,
        jump,
        &mut *first_idx,
        8_i32 + ((1 as core::ffi::c_int) << 4 as core::ffi::c_int) - 2 as core::ffi::c_int,
    );
    for i in 1..segment.segment_num {
        se[0] += se[i];
    }
    se[0] = ((-se[0] as core::ffi::c_longlong * segment.segment_num_inv as core::ffi::c_longlong) >> 31)
        as core::ffi::c_int;
    for i in 1..segment.segment_num {
        se[i] += se[0];
    }
    moving_average(se, data_sft);
    for sft in &mut data_sft[..segment.segment_num] {
        *sft = if *sft < -10_i32 * ((1 as core::ffi::c_int) << 8 as core::ffi::c_int) {
            -10_i32 * ((1 as core::ffi::c_int) << 8 as core::ffi::c_int)
        } else if *sft > 4_i32 * ((1 as core::ffi::c_int) << 8 as core::ffi::c_int) {
            4_i32 * ((1 as core::ffi::c_int) << 8 as core::ffi::c_int)
        } else {
            *sft
        };
        *sft = -*sft;
    }
    avg
}

pub fn lhdc_freq_shift_apply_encode(
    segment: &mut SegmentSettings,
    power_of_2_tbl: &mut [i32],
    data_sft: &mut [i32],
    data: &mut [i32],
    size: i32,
) {
    let mut s: usize = 0;
    let mut power2: f32;
    let mut fixed_power2;
    let mut index_top;
    let mut index_bottom;
    let mut swath = 0;
    while s < segment.segment_num && swath < size {
        power2 = if data_sft[s] >= 0_i32 {
            power_of_2_tbl[(data_sft[s] >> (8_i32 - 4 as core::ffi::c_int)) as usize] as f32
                * 0.000_000_953_674_3_f32
        } else {
            power_of_2_tbl[(((1_i32 << 8 as core::ffi::c_int) * 10 as core::ffi::c_int
                + 15_i32
                + data_sft[s])
                >> (8_i32 - 4 as core::ffi::c_int)) as usize] as f32
                * 0.000_000_000_931_322_6_f32
        };
        fixed_power2 = (power2 * ((1 as core::ffi::c_longlong) << 26_i32) as f32) as i32;
        index_top = if segment.segment_offset[s + 1] as i32 >= size {
            size
        } else {
            segment.segment_offset[s + 1] as _
        };
        index_bottom = if segment.segment_offset[s] as i32 >= size {
            size
        } else {
            segment.segment_offset[s] as _
        };
        let cf = &mut data[segment.segment_offset[s]..];
        swath = index_top - index_bottom;
        for cf_elem in cf.iter_mut().take(swath as _) {
            *cf_elem = ((*cf_elem as core::ffi::c_longlong * fixed_power2 as core::ffi::c_longlong) >> 26_i32)
                as core::ffi::c_int;
        }
        s += 1;
    }
}
static LOG_2_TABLE: [u32; 64] = [
    87720, 87720, 86696, 86256, 89768, 90080, 89328, 89492, 88744, 88600, 89056, 88944, 88304,
    88164, 88468, 88328, 83624, 83696, 83480, 83540, 83936, 83768, 83824, 83120, 83184, 83004,
    83044, 83372, 83348, 83416, 83208, 83320, 82600, 82564, 82672, 82476, 82456, 82544, 82516,
    82828, 82912, 82904, 82744, 82716, 82800, 82768, 82096, 82064, 82160, 82140, 81980, 81944,
    82020, 81984, 82348, 82312, 82324, 82428, 82392, 82208, 82184, 82192, 82296, 82240,
];
fn enc_lossy_check_parameter(ms: i32, khz: i32, resolution: i32) -> i32 {
    if resolution != 16_i32 && resolution != 24 as core::ffi::c_int {
        return -1;
    }
    match ms {
        25 | 75 | 100 => return -1,
        _ => (),
    }
    match khz {
        8000 | 16000 | 24000 | 32000 => return -1,
        _ => (),
    }
    0
}

pub fn lhdc_enc_lossy_start(
    ctx: &mut Context,
    ch_num: i32,
    resolution: i32,
    khz: i32,
    ms: i32,
    enc_pixel_size_byte: i32,
) -> i32 {
    let fdata_all_buffer = ctx.ebuffer();
    if khz > fdata_all_buffer.khz_top || ms > fdata_all_buffer.ms_top {
        return -1_i32;
    }
    if enc_lossy_check_parameter(ms, khz, resolution) < 0_i32 {
        return -1_i32;
    }
    (*fdata_all_buffer).reset(ms, khz, resolution, ch_num, enc_pixel_size_byte as usize)
}
fn calc_header_data_size(
    header_data_size: &mut i32,
    valid_num: i32,
    segment_num: i32,
    fdata_quant_rem_data_size: i32,
) {
    *header_data_size = 1_i32;
    *header_data_size += 1_i32;
    *header_data_size += 9_i32;
    *header_data_size += 4_i32;
    *header_data_size += valid_num;
    *header_data_size += segment_num - 1_i32;
    *header_data_size += fdata_quant_rem_data_size;
}
fn calc_segment_value(khz: i32, ms: i32, bit_rate: i32, pixel_num: i32) -> i32 {
    let sg_r: i32;
    if (khz == 44100_i32 || khz == 48000 as core::ffi::c_int)
        && (ms == 100_i32 || ms == 50 as core::ffi::c_int)
    {
        sg_r = if bit_rate > 119999_i32 {
            SEGMENT_RATE_480_HR as i32
        } else if bit_rate > 95999_i32 {
            SEGMENT_RATE_480 as i32
        } else {
            SEGMENT_RATE_480_LB as i32
        };
    } else if khz == 96000_i32 && ms == 50 as core::ffi::c_int {
        sg_r = 960_i32;
    } else if khz == 192000_i32 && ms == 50 as core::ffi::c_int {
        sg_r = 1920_i32;
    } else {
        sg_r = pixel_num;
    }
    sg_r
}
fn cale_offset_size(offset_size: &mut i32, frm_byte: core::ffi::c_int) {
    *offset_size *= frm_byte;
    *offset_size = if *offset_size >= 1_i32 << 9 as core::ffi::c_int {
        (1_i32 << 9 as core::ffi::c_int) - 1 as core::ffi::c_int
    } else {
        *offset_size
    };
}
fn calc_target_range(
    target_range: &mut i32,
    target_range_bottom: &mut i32,
    enc_pixel_size_byte: i32,
    header_data_size: i32,
    bottom: i32,
) {
    *target_range = enc_pixel_size_byte * 8_i32 - header_data_size;
    *target_range_bottom = *target_range * bottom / 100_i32;
    *target_range_bottom = if *target_range - *target_range_bottom < 24_i32 {
        *target_range - 24_i32
    } else {
        *target_range_bottom
    };
}

pub fn lhdc_enc_lossy_frame_length_program(
    enc_pixel_size_byte: i32,
    fdata_all_buffer: &mut fdata_all_buffer_struct,
) -> i32 {
    let khz = fdata_all_buffer.khz;
    let ms = fdata_all_buffer.ms;
    let enc_pixel_size_byte_bottom = 30_i32 * fdata_all_buffer.ms / 100 as core::ffi::c_int;
    let enc_pixel_size_byte_top = 2000_i32;
    if enc_pixel_size_byte < enc_pixel_size_byte_bottom
        || enc_pixel_size_byte > enc_pixel_size_byte_top
    {
        return -1_i32;
    }
    let frame_number = enc_pixel_size_byte * 100_i32 / fdata_all_buffer.ms;
    let segment_cutoff = segment_cutoff();
    let mut index = 0;
    while frame_number > segment_cutoff[index].size {
        if index == 17 {
            break;
        }
        index += 1;
    }
    fdata_all_buffer.segment_cutoff = segment_cutoff[index].cutoff * fdata_all_buffer.ms / 100_i32;
    if fdata_all_buffer.segment_cutoff > fdata_all_buffer.pixel_num {
        fdata_all_buffer.segment_cutoff = fdata_all_buffer.pixel_num;
    }
    fdata_all_buffer.level_table.init(enc_pixel_size_byte, fdata_all_buffer.resolution, khz, ms);
    let bit_rate = enc_pixel_size_byte * 8_i32 * 10000 as core::ffi::c_int / fdata_all_buffer.ms;
    let bottom = if fdata_all_buffer.ms < 100_i32 {
        90_i32
    } else if bit_rate > 119999_i32 {
        96_i32
    } else {
        95_i32
    };
    if !fdata_all_buffer.segment_setting.init(calc_segment_value(
        khz,
        ms,
        bit_rate,
        fdata_all_buffer.pixel_num,
    ) as usize)
    {
        return -1_i32;
    }
    let read_table1 = init_frequency_table(0_i32);
    let read_table2 = init_frequency_table(1_i32);
    index = 0;
    while index < 3 {
        fdata_all_buffer.init_fq_tbl_01[index] =
            (read_table1[index] >> 2_i32) ^ 21930 as core::ffi::c_int as u32;
        fdata_all_buffer.init_fq_tbl_02[index] =
            (read_table2[index] >> 2_i32) ^ 21930 as core::ffi::c_int as u32;
        index += 1;
    }
    let read_table3 = power_of_2_table();
    index = 0;
    while index < 161 {
        fdata_all_buffer.power_of_2_tbl[index] =
            ((read_table3[index]).wrapping_sub(85426866_i32)) ^ 1437226410_i32;
        index += 1;
    }
    index = 0;
    while (index as core::ffi::c_ulong)
        < (::core::mem::size_of::<[u32; 64]>() as core::ffi::c_ulong)
            .wrapping_div(::core::mem::size_of::<u32>() as core::ffi::c_ulong)
    {
        fdata_all_buffer.logarithm_by_2_tbl[index] =
            (LOG_2_TABLE[index] >> 2_i32) ^ 21930 as core::ffi::c_int as u32;
        index += 1;
    }
    fdata_all_buffer.fdata_quant_rem_data_size =
        if enc_pixel_size_byte < 160_i32 { 0 as core::ffi::c_int } else { 4 as core::ffi::c_int };
    fdata_all_buffer.fdata_quant_rem_data_size_top = if enc_pixel_size_byte < 160_i32 {
        0_i32
    } else {
        ((1_i32 << 4 as core::ffi::c_int) - 1 as core::ffi::c_int) << 7 as core::ffi::c_int
    };
    cale_offset_size(
        &mut fdata_all_buffer.fdata_ch_buffer[0].offset_size,
        fdata_all_buffer.enc_pixel_size_byte / enc_pixel_size_byte,
    );
    cale_offset_size(
        &mut fdata_all_buffer.fdata_ch_buffer[1].offset_size,
        fdata_all_buffer.enc_pixel_size_byte / enc_pixel_size_byte,
    );
    fdata_all_buffer.enc_pixel_size_byte = enc_pixel_size_byte;
    fdata_all_buffer.enc_pixel_size_bits = enc_pixel_size_byte << 3_i32;
    calc_header_data_size(
        &mut fdata_all_buffer.header_data_size,
        fdata_all_buffer.valid_num,
        fdata_all_buffer.segment_setting.segment_num as _,
        fdata_all_buffer.fdata_quant_rem_data_size,
    );
    calc_target_range(
        &mut fdata_all_buffer.target_range,
        &mut fdata_all_buffer.target_range_bottom,
        enc_pixel_size_byte,
        fdata_all_buffer.header_data_size,
        bottom,
    );
    fdata_all_buffer.esti_diff_value = if fdata_all_buffer.resolution == 16_i32 {
        if fdata_all_buffer.enc_pixel_size_byte <= 125_i32 {
            69468_i32
                - ((69468_i32 - 65864 as core::ffi::c_int)
                    * (fdata_all_buffer.enc_pixel_size_byte - 50_i32)
                    + 32_i32)
                    / (125_i32 - 50 as core::ffi::c_int)
        } else {
            66192_i32
        }
    } else if fdata_all_buffer.enc_pixel_size_byte <= 120_i32 {
        68812_i32
            - ((68812_i32 - 65536 as core::ffi::c_int) * (fdata_all_buffer.enc_pixel_size_byte - 50_i32)
                + 35_i32)
                / (120_i32 - 50 as core::ffi::c_int)
    } else {
        65536_i32
    };
    0_i32
}

pub fn lhdc_enc_lossy_frame_size_program(ctx: &mut Context) -> i32 {
    ctx.ebuffer().pixel_num
}
fn lhdc_enc_lossy_freq_process(fdata_all_buffer: &mut fdata_all_buffer_struct, channel: i32) {
    let data_in = <[i32]>::mut_from_bytes(
        fdata_all_buffer.frequency_mem.frequency_buffers.fft_coef_in.as_mut_bytes(),
    )
    .unwrap();
    let lossy_fdata_buf = &mut fdata_all_buffer.fdata_enc_buffer.lossy_fdata_buf;
    let pixel_num = fdata_all_buffer.pixel_num;
    let mut pixel_num_top = 1_i32 << 30 as core::ffi::c_int;
    let mut pixel_num_bottom = -(1_i32 << 30 as core::ffi::c_int);
    let ecb_pcm = &mut (fdata_all_buffer.frequency_mem).frequency_buffers.freq_data
        [fdata_all_buffer.pixel_ov_num as usize..];
    data_in[..pixel_num as usize].copy_from_slice(&ecb_pcm[..pixel_num as usize]);
    lhdc_enc_lossy_frequency_operation(
        &mut fdata_all_buffer.frequency_mem,
        channel,
        lossy_fdata_buf,
    );
    fdata_all_buffer.fdata_enc_buffer.lossy_fdata_ave = lhdc_enc_freq_shift(
        &mut fdata_all_buffer.segment_setting,
        fdata_all_buffer.segment_cutoff,
        lossy_fdata_buf,
        &mut fdata_all_buffer.fdata_enc_buffer.freqency_segment,
        &mut fdata_all_buffer.fdata_enc_buffer.lossy_jump_adus_parameter,
        &mut fdata_all_buffer.fdata_enc_buffer.lossy_jump_adus_num,
        &mut fdata_all_buffer.logarithm_by_2_tbl,
    );
    if fdata_all_buffer.fdata_enc_buffer.lossy_jump_adus_num
        != 8_i32 + ((1 as core::ffi::c_int) << 4 as core::ffi::c_int) - 2 as core::ffi::c_int + 1_i32
    {
        lhdc_freq_shift_apply_encode(
            &mut fdata_all_buffer.segment_setting,
            &mut fdata_all_buffer.power_of_2_tbl,
            &mut fdata_all_buffer.fdata_enc_buffer.freqency_segment,
            lossy_fdata_buf,
            pixel_num,
        );
    }
    fdata_all_buffer.fdata_enc_buffer.jump_level_top = 0_i32;
    if fdata_all_buffer.resolution == 24_i32 {
        let mut index = 0_i32;
        while index < pixel_num {
            let lossy_fdata = lossy_fdata_buf[index as usize];
            pixel_num_top = if lossy_fdata > pixel_num_top { lossy_fdata } else { pixel_num_top };
            pixel_num_bottom =
                if lossy_fdata < pixel_num_bottom { lossy_fdata } else { pixel_num_bottom };
            index += 1;
        }
        pixel_num_bottom = -pixel_num_bottom;
        pixel_num_top =
            if pixel_num_bottom > pixel_num_top { pixel_num_bottom } else { pixel_num_top };
        if pixel_num_top > 0_i32 {
            index = 0_i32;
            loop {
                let level = fdata_all_buffer.level_table.level(index);
                if pixel_num_top as f32 * level <= (1_i32 << 30 as core::ffi::c_int) as f32 {
                    break;
                }
                index += 1;
            }
            fdata_all_buffer.fdata_enc_buffer.jump_level_top = index;
        }
    }
}
fn lhdc_enc_lossy_freq_level(fdata_all_buffer: &mut fdata_all_buffer_struct, level: f32) -> i32 {
    let in_0 = &mut fdata_all_buffer.fdata_enc_buffer.lossy_fdata_buf;
    let cutoff = fdata_all_buffer.segment_cutoff;
    let out = &mut fdata_all_buffer.fdata_enc_buffer.level.fdata_q_quant;
    let fdata_q_len = &mut fdata_all_buffer.fdata_enc_buffer.level.fdata_q_len;

    let mut index: i32;
    let level_ = (level * (1_i32 << 30 as core::ffi::c_int) as f32) as core::ffi::c_int;
    let n_level_ = -level_;
    let out05_multiply_2 = (2.0f32 * 0.5f32 / level) as core::ffi::c_longlong;
    let n_out05_multiply_2 = -out05_multiply_2;
    let mut sernum = 0_i32;
    let out_tmp = &mut out[(fdata_all_buffer.pixel_num - cutoff) as usize..];

    for i in 0..(cutoff as usize) {
        let in_ = in_0[cutoff as usize - 1 - i] as i64;
        let in_multiply_2_0 = in_ << 1;
        if in_multiply_2_0 >= out05_multiply_2 {
            out_tmp[i] = ((in_ * level_ as i64 + (1i64 << (30 - 1))) >> 30) as i32;
            sernum += 1;
        } else if in_multiply_2_0 <= n_out05_multiply_2 {
            out_tmp[i] = ((in_ * n_level_ as i64 + (1i64 << (30 - 1))) >> 30) as i32;
            sernum += 1;
        } else {
            out_tmp[i] = 0;
        }
    }
    *fdata_q_len = sernum;
    index = fdata_all_buffer.pixel_num - cutoff;
    while index < fdata_all_buffer.pixel_num && out[index as usize] == 0_i32 {
        index += 1;
    }
    if index == fdata_all_buffer.pixel_num {
        return 0_i32;
    }
    fdata_all_buffer.pixel_num - index + (index & 1_i32)
}
fn fast_calc_step(
    jump: f32,
    per_pixel_number: i32,
    pixel_number: i32,
    offset_size: i32,
    per_offset_size: i32,
) -> f32 {
    let tmp: f32 = jump * 0.8f32;
    let sign_tmp1: i32 = per_pixel_number - pixel_number;
    let sign_tmp2: i32 = offset_size - per_offset_size;
    (if sign_tmp2 > 0_i32 {
        (sign_tmp1 / sign_tmp2) as f32 * 0.2f32
    } else {
        (-sign_tmp1 / -sign_tmp2) as f32 * 0.2f32
    }) + tmp
}
fn lhdc_enc_lossy_nbyte_estimation(
    fdata_all_buffer: &mut fdata_all_buffer_struct,
    channel: usize,
) -> i32 {
    let fdata_ch_buffer = &mut fdata_all_buffer.fdata_ch_buffer[channel];
    let target_range = fdata_all_buffer.target_range;
    let target_range_bottom = fdata_all_buffer.target_range_bottom;

    let mut offset = fdata_ch_buffer.offset_step;
    let mut offset_tbl_size_bottom = (1_i32 << 9 as core::ffi::c_int) - 1 as core::ffi::c_int;
    let mut offset_tbl_size_top = -1_i32;
    let mut offset_size = fdata_ch_buffer.offset_size;
    let mut per_offset_size = offset_size;
    if offset_size < fdata_all_buffer.fdata_enc_buffer.jump_level_top {
        offset_size = fdata_all_buffer.fdata_enc_buffer.jump_level_top;
        per_offset_size = offset_size;
    }
    let mut pixel_number = 0_i32;
    loop {
        let mut level = fdata_all_buffer.level_table.level(offset_size);
        let tmp_out = lhdc_enc_lossy_freq_level(&mut *fdata_all_buffer, level);
        let mut ncz_in = &mut fdata_all_buffer.fdata_enc_buffer.level.valid;
        *ncz_in = tmp_out;
        if *ncz_in < 4_i32 {
            *ncz_in = 4_i32;
        }
        let per_pixel_number = pixel_number;
        pixel_number = lhdc_enc_nbyte_esti(&mut *fdata_all_buffer);
        if pixel_number > target_range {
            offset_tbl_size_top = offset_size;
            if offset_tbl_size_top == offset_tbl_size_bottom - 1_i32 {
                offset_size = offset_tbl_size_bottom;
                level = fdata_all_buffer.level_table.level(offset_size);
                let tmp_out = lhdc_enc_lossy_freq_level(&mut *fdata_all_buffer, level);
                ncz_in = &mut fdata_all_buffer.fdata_enc_buffer.level.valid;
                *ncz_in = tmp_out;
                if *ncz_in < 4_i32 {
                    *ncz_in = 4_i32;
                }
                pixel_number = lhdc_enc_nbyte_esti(&mut *fdata_all_buffer);
                break;
            }
        } else {
            if pixel_number >= target_range_bottom {
                break;
            }
            if offset_size <= fdata_all_buffer.fdata_enc_buffer.jump_level_top {
                break;
            }
            offset_tbl_size_bottom = offset_size;
            if offset_size > fdata_all_buffer.level_table.start + 1_i32 {
                if offset_tbl_size_bottom == offset_tbl_size_top + 1_i32 {
                    break;
                }
            } else {
                offset_size -= 1;
                continue;
            }
        }
        if offset_size != per_offset_size {
            offset = fast_calc_step(
                offset,
                per_pixel_number,
                pixel_number,
                offset_size,
                per_offset_size,
            );
        }
        per_offset_size = offset_size;
        offset_size += ((pixel_number - target_range) as f32 / offset) as i32;
        if offset_size == per_offset_size {
            offset_size = if pixel_number < target_range {
                offset_size -= 1;
                offset_size
            } else {
                offset_size += 1;
                offset_size
            };
        }
        offset_size = if offset_size <= fdata_all_buffer.level_table.start {
            fdata_all_buffer.level_table.start + 1_i32
        } else {
            offset_size
        };
        if offset_size > offset_tbl_size_bottom {
            offset_size = offset_tbl_size_bottom - 1_i32;
        } else if offset_size <= offset_tbl_size_top {
            offset_size = offset_tbl_size_top + 1_i32;
        }
    }
    let fdata_ch_buffer = &mut fdata_all_buffer.fdata_ch_buffer[channel];
    fdata_ch_buffer.offset_size = offset_size;
    fdata_ch_buffer.offset_step = offset;
    pixel_number
}
fn bit_write_uint(
    nbyte_program: &mut BitOutputter,
    fdata_enc_buffer: &mut fdata_enc_buffer_struct,
    data_offset: i32,
    segment_num: i32,
    valid_num: i32,
    fdata_quant_rem_data_size: i32,
) -> Result<()> {
    let fdata_buffer = &mut fdata_enc_buffer.level;
    let result: i32 = 0 as core::ffi::c_int;
    nbyte_program.write_bits(0, 1)?;
    nbyte_program.write_bits(data_offset as u32, 9)?;
    nbyte_program.write_bits((fdata_enc_buffer.lossy_jump_adus_num as u32).wrapping_sub(8), 4)?;
    if segment_num - 1_i32 != 0 {
        let mut index: i32 = 0 as core::ffi::c_int;
        while index < segment_num - 1 && result == 0 {
            nbyte_program
                .write_bits(fdata_enc_buffer.lossy_jump_adus_parameter[index as usize] as u32, 1)?;
            index += 1;
        }
    }
    nbyte_program.write_bits(((fdata_buffer.valid >> 1) - 1) as u32, valid_num as u32)?;
    nbyte_program.write_bits(fdata_buffer.symbol_flag as u32, 1)?;
    nbyte_program
        .write_bits(fdata_buffer.fdata_q_quant_rem[0] as u32, fdata_quant_rem_data_size as u32)?;
    Ok(())
}
fn lhdc_enc_lossy_nbyte_out(
    fdata_all_buffer: &mut fdata_all_buffer_struct,
    enc_arith: &mut arith::EncoderBase,
    nbyte_program: &mut BitOutputter,
    channel: i32,
) -> Result<i32> {
    let fdata_ch_buffer: &mut fdata_ch_buffer_struct =
        &mut fdata_all_buffer.fdata_ch_buffer[channel as usize];
    let fdata_enc_buffer: &mut fdata_enc_buffer_struct = &mut fdata_all_buffer.fdata_enc_buffer;
    bit_write_uint(
        nbyte_program,
        fdata_enc_buffer,
        fdata_ch_buffer.offset_size,
        fdata_all_buffer.segment_setting.segment_num as _,
        fdata_all_buffer.valid_num,
        fdata_all_buffer.fdata_quant_rem_data_size,
    )?;

    lhdc_enc_nbyte_out(fdata_all_buffer, enc_arith, nbyte_program)?;
    let fdata_ch_buffer: &mut fdata_ch_buffer_struct =
        &mut fdata_all_buffer.fdata_ch_buffer[channel as usize];
    let fdata_enc_buffer: &mut fdata_enc_buffer_struct = &mut fdata_all_buffer.fdata_enc_buffer;

    let fdata_buffer: &mut fdata_buffer_struct = &mut fdata_enc_buffer.level;
    let mut nbyte = fdata_buffer.fdata_q_true_len;
    let mut word = 0;
    let mut bit_len = 32;
    let non_zero_counter = fdata_buffer.valid;
    let mut index = 0_i32;
    let mut read_byte_number = index;
    let read_data = &mut fdata_buffer.fdata_q_quant
        [(fdata_all_buffer.pixel_num - fdata_buffer.valid) as usize..];
    for (i, (read_data_byte, read_data)) in fdata_buffer
        .fdata_q_quant_rem
        .iter()
        .copied()
        .zip(read_data.iter().copied())
        .take(non_zero_counter as usize)
        .enumerate()
    {
        if bit_len < read_data_byte as u32 + 1 {
            nbyte_program.write_bits(word, 32 - bit_len)?;
            bit_len = 32;
        }
        if read_data_byte != 0 {
            word = (word << read_data_byte)
                | read_data as u32 & (0xffffffffu32 >> (32 - read_data_byte));
            bit_len -= read_data_byte as u32;
            read_byte_number += read_data_byte;
        }
        if read_data != 0 {
            bit_len -= 1;
            word <<= 1;
            if fdata_enc_buffer.lossy_fdata_buf[non_zero_counter as usize - 1 - i] < 0 {
                word |= 1
            }
        }
    }
    nbyte_program.write_bits(word, 32_u32.wrapping_sub(bit_len))?;
    read_byte_number += fdata_buffer.fdata_q_len;
    if read_byte_number < 24_i32 {
        nbyte_program.write_bits(0, (24_i32 - read_byte_number) as u32)?;
        read_byte_number = 24_i32;
    }
    nbyte += read_byte_number;
    if nbyte > fdata_all_buffer.target_range {
        return Err(Error::TargetRange);
    }
    let level = (fdata_all_buffer.level_table.level(fdata_ch_buffer.offset_size)
        * (1_i32 << 30) as f32) as core::ffi::c_int;
    let n_level = -level;
    index = fdata_all_buffer.pixel_num - 1;
    while nbyte < fdata_all_buffer.target_range
        && index >= fdata_all_buffer.pixel_num - fdata_buffer.valid
    {
        if fdata_buffer.fdata_q_quant[index as usize] != 0 {
            let lossy_fdata =
                fdata_enc_buffer.lossy_fdata_buf[(fdata_all_buffer.pixel_num - 1 - index) as usize];
            let read_data_count = if lossy_fdata >= 0_i32 {
                (lossy_fdata as core::ffi::c_longlong * level as core::ffi::c_longlong) as i32 >> 30
            } else {
                (lossy_fdata as core::ffi::c_longlong * n_level as core::ffi::c_longlong) as i32 >> 30
            };
            nbyte_program.write_bits(
                (fdata_buffer.fdata_q_quant[index as usize] - read_data_count) as u32,
                1,
            )?;
            nbyte += 1;
        }
        index -= 1;
    }
    nbyte += fdata_all_buffer.header_data_size;
    while (nbyte + 8_i32) < fdata_all_buffer.enc_pixel_size_bits {
        nbyte_program.write_bits(0, 8)?;
        nbyte += 8_i32;
    }
    nbyte_program.zero_pad()?;
    Ok(fdata_all_buffer.enc_pixel_size_bits)
}

pub fn lhdc_enc_top(
    data_in: &[i32],
    data_l_out_size: &mut i32,
    data_r_out_size: &mut i32,
    ctx: &mut Context,
) {
    let data_l_out = &mut ctx.encoded_data_ch0;
    let data_r_out = &mut ctx.encoded_data_ch1;
    let fdata_all_buffer = &mut ctx.ebuffer;
    let mut channel = 0_i32;
    let mut outs = [data_l_out, data_r_out];
    let mut sizes = [data_l_out_size, data_r_out_size];
    while channel < fdata_all_buffer.ch_num {
        let data_l_out = &mut outs[channel as usize];
        let data_l_out_size = &mut sizes[channel as usize];
        let ori_input_pcm = &mut (fdata_all_buffer.frequency_mem).frequency_buffers.freq_data
            [fdata_all_buffer.pixel_ov_num as usize..];
        let pixel_num = fdata_all_buffer.pixel_num;
        ori_input_pcm[..pixel_num as usize].copy_from_slice(
            &data_in[(pixel_num * channel) as usize..(pixel_num * (channel + 1)) as usize],
        );
        (fdata_all_buffer.frequency_mem)
            .frequency_process_ch_num
            .set_offset((fdata_all_buffer.freqency_ov_top * channel) as usize);
        lhdc_enc_lossy_freq_process(fdata_all_buffer, channel);
        lhdc_enc_lossy_nbyte_estimation(&mut *fdata_all_buffer, channel as usize);
        let mut bo;

        loop {
            bo = BitOutputter::new(*data_l_out);

            let mut density_size_enc =
                lhdc_enc_lossy_nbyte_out(fdata_all_buffer, &mut ctx.enc_arith_s, &mut bo, channel)
                    .unwrap_or(-2);
            if density_size_enc > 0_i32 {
                break;
            }
            let fdata_buffer: &mut fdata_buffer_struct =
                &mut fdata_all_buffer.fdata_enc_buffer.level;
            density_size_enc = fdata_buffer.fdata_q_true_len - fdata_buffer.fdata_q_esti_len;
            let offset = fdata_all_buffer.pixel_num - fdata_buffer.valid;
            let mut data = &fdata_buffer.fdata_q_quant[offset as usize..];
            let mut read_data_byte: &mut [i32] = &mut fdata_buffer.fdata_q_quant_rem;
            let mut bit_len = 0_i32;
            loop {
                if data[0] != 0 && data[1] != 0 {
                    fdata_buffer.fdata_q_len -= 2;
                    density_size_enc -= 2;
                    bit_len += 40;
                } else if data[0] == 0 && data[1] == 0 {
                    bit_len += 8_i32;
                } else if data[0] != 0 && data[1] == 0 || data[0] == 0 && data[1] != 0 {
                    fdata_buffer.fdata_q_len -= 1;
                    density_size_enc -= 1;
                    bit_len += 20_i32;
                    bit_len += 4_i32;
                }
                density_size_enc -= read_data_byte[0] + read_data_byte[1];
                fdata_buffer.valid -= 2_i32;
                data = &data[2..];
                read_data_byte = &mut read_data_byte[2..];
                if density_size_enc <= bit_len >> 4_i32 {
                    break;
                }
            }
            lhdc_enc_nbyte_esti(fdata_all_buffer);
        }

        **data_l_out_size = bo.clear().expect("Error unhandled in original code") as i32;
        channel += 1;
    }
    fdata_all_buffer.frame_count += 1;
}
fn lhdc_enc_lossy_nbyte_num_read(channel: i32, khz: i32, ms: i32, bitrate: i32) -> i32 {
    let mut frame_len = bitrate * ms / (8_i32 * 1000 * 10 * channel);
    if khz == 44100_i32 {
        frame_len = bitrate * ms / (735_i32 * 100 * channel);
    }
    frame_len
}

pub fn lhdc_enc_init(
    channel: i32,
    resolution: i32,
    khz: i32,
    ms: i32,
    bitrate: i32,
    ctx: &mut Context,
) -> Result<()> {
    if !(1_i32..=8_i32).contains(&channel)
        || resolution != 16 && resolution != 24
        || khz != 8000
            && khz != 16000
            && khz != 24000
            && khz != 32000
            && khz != 44100
            && khz != 48000
            && khz != 96000
            && khz != 192000
        || ms != 50
        || (bitrate < 0 || bitrate > 1000000)
    {
        return Err(Error::Encoder);
    }
    let frame_len: i32 = lhdc_enc_lossy_nbyte_num_read(channel, khz, ms, bitrate);
    ctx.hdr_s().enc_frm_len_provided = frame_len;
    if lhdc_enc_lossy_start(ctx, channel, resolution, khz, ms, frame_len) < 0_i32 {
        return Err(Error::Encoder);
    }
    Ok(())
}

pub fn lhdc_enc_get_samples_per_frame(s_fps: &mut i32, ctx: &mut Context) {
    *s_fps = lhdc_enc_lossy_frame_size_program(ctx);
}

pub fn lhdc_enc_set_bitrate(bitrate: i32, ctx: &mut Context) -> Result<()> {
    if bitrate < 0 || bitrate > 1000000 {
        return Err(Error::BadParams);
    }
    let fdata_all_buffer: &mut fdata_all_buffer_struct = ctx.ebuffer();
    let channel = fdata_all_buffer.ch_num;
    let khz = fdata_all_buffer.khz;
    let ms = fdata_all_buffer.ms;
    let header: &mut Header = ctx.hdr_s();
    let frame_len: i32 = lhdc_enc_lossy_nbyte_num_read(channel, khz, ms, bitrate);
    header.enc_frm_len_provided = frame_len;
    header.enc_frm_len_need_update = 1;
    if lhdc_enc_lossy_frame_length_program(frame_len, ctx.ebuffer()) < 0 {
        return Err(Error::BadParams);
    }
    Ok(())
}

static XOR_MASK: [u8; 8] = [0xff, 0xe7, 0x7a, 0xb3, 0xda, 0xe5, 0xcd, 0x73];
static SCRAMBLE_ORDER: [[u8; 8]; 2] = [[4, 0, 1, 5, 7, 3, 2, 6], [6, 3, 7, 0, 2, 1, 4, 5]];
fn encoded_data_encrypt(encoded_data: &mut [u32]) {
    let mut index: i32;
    let byte = encoded_data.as_mut_bytes();
    let mut tmp_byte: [core::ffi::c_uchar; 8] = [0, 0, 0, 0, 0, 0, 0, 0];
    let scramble_index = byte[8] & 0x1;
    index = 0;
    while index < 8 {
        tmp_byte[index as usize] = byte[index as usize] ^ XOR_MASK[index as usize];
        index += 1;
    }
    index = 0;
    while index < 8 {
        byte[index as usize] =
            tmp_byte[SCRAMBLE_ORDER[scramble_index as usize][index as usize] as i32 as usize];
        index += 1;
    }
}

pub fn lhdc_enc_encode(
    input_buffer_top: &mut [i32],
    mut output_buffer_top: &mut [u8],
    output_frame_size: &mut i32,
    ctx: &mut Context,
) {
    let mut input_buffer_size_ch0: i32 = 0;
    let mut input_buffer_size_ch1: i32 = 1;
    let mut density_size_workable: i32 = 0;
    let mut fdata_all_buffer = &mut ctx.ebuffer;
    let header = &mut ctx.hdr_s;
    if fdata_all_buffer.frame_cnt < 960 {
        fdata_all_buffer.frame_cnt += 1;
    }
    let header_size = enc_process_header(
        &mut *header,
        fdata_all_buffer.ch_num,
        &mut density_size_workable,
        output_buffer_top,
    );
    output_buffer_top = &mut output_buffer_top[header_size as usize..];
    if header.enc_frm_len_need_update != 0 {
        lhdc_enc_lossy_frame_length_program(density_size_workable, fdata_all_buffer);
        header.enc_frm_len_need_update = 0;
    }
    lhdc_enc_top(input_buffer_top, &mut input_buffer_size_ch0, &mut input_buffer_size_ch1, ctx);
    let ch0 = &mut ctx.encoded_data_ch0;
    let ch1 = &mut ctx.encoded_data_ch1;
    fdata_all_buffer = &mut ctx.ebuffer;

    encoded_data_encrypt(ch0);
    if fdata_all_buffer.ch_num >= 2_i32 {
        encoded_data_encrypt(ch1);
    }
    let copy_len = fdata_all_buffer.enc_pixel_size_byte as usize;
    output_buffer_top[0..copy_len].copy_from_slice(&ch0.as_bytes()[0..copy_len]);
    if fdata_all_buffer.ch_num >= 2_i32 {
        output_buffer_top[copy_len..copy_len * 2].copy_from_slice(&ch1.as_bytes()[0..copy_len]);
    }
    *output_frame_size =
        header_size + fdata_all_buffer.ch_num * fdata_all_buffer.enc_pixel_size_byte;
}
