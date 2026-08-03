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

#![allow(non_camel_case_types)]
use alloc::boxed::Box;
use alloc::vec::Vec;
#[allow(unused_imports)]
use alloc::vec;
use crate::common::lhdc_level::LevelTable;
use crate::enc::process::lhdc_enc_lossy_frame_length_program;
use crate::lhdc_enc::lhdc_enc_freq_process::*;
use crate::lhdc_enc::lhdc_enc_header::SegmentSettings;
use crate::math::log_2;
use zerocopy::IntoBytes;

pub struct fdata_buffer_struct {
    pub fdata_q_quant: Box<[i32]>,
    pub fdata_q_quant_rem: Box<[i32]>,
    pub fdata_q_quant_bit_num: Box<[i32]>,
    pub fdata_q_quant_2_symbol: Box<[u32]>,
    pub valid: core::ffi::c_int,
    pub symbol_flag: core::ffi::c_int,
    pub fdata_q_size: core::ffi::c_int,
    pub fdata_q_len: core::ffi::c_int,
    pub fdata_q_esti_len: core::ffi::c_int,
    pub fdata_q_true_len: core::ffi::c_int,
}

fn copybox<T: Copy>(v: T, count: usize) -> Box<[T]> {
    core::iter::repeat_n(v, count).collect()
}

impl fdata_buffer_struct {
    pub fn new(max_frame_size: usize, half_max_lossy_umem8_len: usize) -> Self {
        Self {
            fdata_q_quant: copybox(0, max_frame_size),
            fdata_q_quant_rem: copybox(0, max_frame_size),
            fdata_q_quant_bit_num: copybox(0, half_max_lossy_umem8_len + 1),
            fdata_q_quant_2_symbol: copybox(0, max_frame_size),
            valid: 0,
            symbol_flag: 0,
            fdata_q_size: 0,
            fdata_q_len: 0,
            fdata_q_esti_len: 0,
            fdata_q_true_len: 0,
        }
    }
}

pub struct fdata_enc_buffer_struct {
    pub lossy_fdata_buf: Box<[i32]>,
    pub freqency_segment: [core::ffi::c_int; 32],
    pub lossy_fdata_ave: core::ffi::c_int,
    pub lossy_jump_adus_parameter: [core::ffi::c_uchar; 31],
    pub lossy_jump_adus_num: core::ffi::c_int,
    pub jump_level_top: core::ffi::c_int,
    pub level: fdata_buffer_struct,
}

impl fdata_enc_buffer_struct {
    fn count_calc(ms_max: usize, khz_max: usize, max_frame_size: usize) -> usize {
        let max_frame_size_10ms = khz_max * 100 / 10000;
        if ms_max == 50 && (khz_max == 48000 || khz_max == 96000 || khz_max == 192000) {
            max_frame_size_10ms
        } else if max_frame_size > 480 {
            max_frame_size
        } else {
            480
        }
    }
    pub fn new(
        ms_max: usize,
        khz_max: usize,
        max_frame_size: usize,
        max_lossy_umem8_len: usize,
    ) -> Self {
        Self {
            lossy_fdata_buf: copybox(0, Self::count_calc(ms_max, khz_max, max_frame_size)),
            freqency_segment: [0; 32],
            lossy_fdata_ave: 0,
            lossy_jump_adus_parameter: [0; 31],
            lossy_jump_adus_num: 0,
            jump_level_top: 0,
            level: fdata_buffer_struct::new(max_frame_size, max_lossy_umem8_len / 2),
        }
    }
}

#[derive(Copy, Clone, Default)]
pub struct fdata_ch_buffer_struct {
    pub offset_size: core::ffi::c_int,
    pub offset_step: f32,
}

pub struct fdata_all_buffer_struct {
    pub khz_top: core::ffi::c_int,
    pub ms_top: core::ffi::c_int,
    pub pixel_num_top: core::ffi::c_int,
    pub lossy_symbol_size_top: core::ffi::c_int,
    pub freqency_ov_top: core::ffi::c_int,
    pub khz: core::ffi::c_int,
    pub resolution: core::ffi::c_int,
    pub ch_num: core::ffi::c_int,
    pub pixel_num: core::ffi::c_int,
    pub pixel_ov_num: core::ffi::c_int,
    pub ms: core::ffi::c_int,
    pub valid_num: core::ffi::c_int,
    pub fdata_quant_rem_data_size: core::ffi::c_int,
    pub fdata_quant_rem_data_size_top: core::ffi::c_int,
    pub header_data_size: core::ffi::c_int,
    pub frame_cnt: core::ffi::c_int,
    pub enc_pixel_size_byte: core::ffi::c_int,
    pub enc_pixel_size_bits: core::ffi::c_int,
    pub target_range: core::ffi::c_int,
    pub target_range_bottom: core::ffi::c_int,
    pub esti_diff_value: core::ffi::c_int,
    pub level_table: LevelTable,
    pub segment_setting: SegmentSettings,
    pub segment_cutoff: core::ffi::c_int,
    pub frequency_mem: Top,
    pub fdata_ch_buffer: [fdata_ch_buffer_struct; 2],
    pub fdata_enc_buffer: fdata_enc_buffer_struct,
    pub frame_count: core::ffi::c_int,
    pub init_fq_tbl_01: [u32; 3],
    pub init_fq_tbl_02: [u32; 3],
    pub power_of_2_tbl: [core::ffi::c_int; 161],
    pub logarithm_by_2_tbl: [core::ffi::c_uint; 64],
}

impl fdata_all_buffer_struct {
    pub fn lossy_data_not_change(&mut self) -> &mut [u8] {
        self.fdata_enc_buffer.level.fdata_q_quant_bit_num.as_mut_bytes()
    }
    pub fn new(
        khz_top: i32,
        ms_top: i32,
        max_frame_size: i32,
        max_lossy_umem8_len: i32,
        max_freq_repeating: i32,
    ) -> Self {
        let ms_max = ms_top as usize;
        let khz_max = khz_top as usize;
        Self {
            khz_top,
            ms_top,
            pixel_num_top: max_frame_size,
            lossy_symbol_size_top: max_lossy_umem8_len,
            freqency_ov_top: max_freq_repeating,
            fdata_ch_buffer: [fdata_ch_buffer_struct::default(); 2],
            frequency_mem: Top::new(
                max_frame_size as usize,
                max_freq_repeating as usize,
                //TODO(b/454096420) max_ch
                2,
            ),
            fdata_enc_buffer: fdata_enc_buffer_struct::new(
                ms_max,
                khz_max,
                max_frame_size as usize,
                max_lossy_umem8_len as usize,
            ),

            // Will be set by a `load`/`reset` call
            khz: 0,
            resolution: 0,
            ch_num: 0,
            pixel_num: 0,
            pixel_ov_num: 0,
            ms: 0,
            valid_num: 0,
            fdata_quant_rem_data_size: 0,
            fdata_quant_rem_data_size_top: 0,
            header_data_size: 0,
            frame_cnt: 0,
            enc_pixel_size_byte: 0,
            enc_pixel_size_bits: 0,
            target_range: 0,
            target_range_bottom: 0,
            esti_diff_value: 0,
            level_table: LevelTable::new(),
            segment_setting: SegmentSettings::default(),
            segment_cutoff: 0,
            frame_count: 0,
            init_fq_tbl_01: [0; 3],
            init_fq_tbl_02: [0; 3],
            power_of_2_tbl: [0; 161],
            logarithm_by_2_tbl: [0; 64],
        }
    }
    fn lossy_frame_length_program(&mut self, size: usize) -> i32 {
        lhdc_enc_lossy_frame_length_program(size as i32, self)
    }
    pub fn reset(
        &mut self,
        ms: i32,
        khz: i32,
        resolution: i32,
        ch_num: i32,
        enc_pixel_size_byte: usize,
    ) -> i32 {
        self.ms = ms;
        self.khz = khz;
        self.resolution = resolution;
        self.ch_num = ch_num;
        let khz_calc = if khz == 44100 { 48000 } else { khz };
        let fdata_for_ov = match ms {
            25 => 10,
            50 => 20,
            75 | 100 => 25,
            _ => 0,
        };

        self.pixel_ov_num = khz_calc * fdata_for_ov / 10_000;
        self.pixel_num = khz_calc * ms / 10_000;
        self.valid_num = (log_2(self.pixel_num as u32) - 1) as _;
        let result = self.lossy_frame_length_program(enc_pixel_size_byte);
        if result != 0 {
            return result;
        }
        self.fdata_ch_buffer[0].offset_step = 8704.0f32 / ((1 as core::ffi::c_int) << 9) as f32;
        self.fdata_ch_buffer[1].offset_step = self.fdata_ch_buffer[0].offset_step;
        self.frequency_mem.reset(
            self.pixel_num * 2,
            self.pixel_ov_num,
            31 as core::ffi::c_int - self.resolution,
        );

        if result != 0 {
            return result;
        }
        self.frame_cnt = 0;
        0
    }
}

pub type lhdc_enc_error = core::ffi::c_int;
pub const LHDC_ENC_ERROR: lhdc_enc_error = -1;
pub const LHDC_ENC_OK: lhdc_enc_error = 0;
pub type lhdc_enc_workspace_mode_options = core::ffi::c_uint;
pub const LHDC_ENC_MODE_OPTION_0: lhdc_enc_workspace_mode_options = 0;
