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

use alloc::boxed::Box;
use alloc::vec::Vec;
#[allow(unused_imports)]
use alloc::vec;
use crate::kiss_fft::*;
use zerocopy::{FromBytes, IntoBytes};

use core::f64::consts::PI;

// TODO(b/454096420) dedup
fn copybox<T: Copy>(v: T, count: usize) -> Box<[T]> {
    core::iter::repeat_n(v, count).collect()
}

pub struct FrequencyBuffers {
    pub freq_data: Box<[i32]>,
    pub fft_iv: Box<[i32]>,
    pub fft_coef_in: Box<[Complex]>,
    pub fft_coef_out: Box<[Complex]>,
}

impl FrequencyBuffers {
    pub fn new(max_frame_size: usize, max_frequency_repeating: usize) -> Self {
        Self {
            freq_data: copybox(0, max_frame_size + max_frequency_repeating),
            fft_iv: copybox(0, max_frame_size),
            fft_coef_in: copybox(Default::default(), max_frame_size / 2),
            fft_coef_out: copybox(Default::default(), max_frame_size / 2),
        }
    }
}

pub struct RepeatingChannels {
    offset: usize,
    repeat_size_top: Box<[i32]>,
}

impl RepeatingChannels {
    pub fn new(max_frequency_repeating: usize, ch: usize) -> Self {
        Self { offset: 0, repeat_size_top: copybox(0, max_frequency_repeating * ch) }
    }
    pub fn repeat_size_top(&mut self) -> &mut [i32] {
        &mut self.repeat_size_top[self.offset..]
    }
    pub fn set_offset(&mut self, offset: usize) {
        self.offset = offset
    }
}

pub struct Top {
    pub size: core::ffi::c_int,
    pub repeating: core::ffi::c_int,
    pub offset_num: core::ffi::c_int,
    pub num1: core::ffi::c_int,
    pub num2: core::ffi::c_int,
    pub num3: core::ffi::c_int,
    pub num4: core::ffi::c_int,
    pub kiss_fft_cfg: State,
    pub freqency_win: Box<[i32]>,
    pub pre_twid: Box<[Complex]>,
    pub pos_twid: Box<[Complex]>,
    pub frequency_process_ch_num: RepeatingChannels,
    pub frequency_buffers: FrequencyBuffers,
    pub pos_twid_scalar_shift: core::ffi::c_int,
}

impl Top {
    pub fn reset(&mut self, frequency_size: i32, repeating: i32, sft_num: i32) {
        self.repeating = repeating;
        self.size = frequency_size;
        self.num1 = frequency_size / 2;
        self.num2 = self.num1 / 2;
        self.num3 = repeating / 2;
        self.num4 = (frequency_size - self.num1 - repeating) / 2;
        self.offset_num = sft_num;

        self.resolve()
    }

    // TODO(b/454096420) better name
    fn resolve(&mut self) {
        let num1 = self.num1;
        let num2 = self.num2;
        let sft_num = self.offset_num;
        self.pos_twid_scalar_shift = calc_pos_twid_scale_int_bits(num2);
        let scale = ((1 as core::ffi::c_int) << sft_num) as f32 / num2 as f32;
        let pos_twid_scalar_shift_val = (1 as core::ffi::c_longlong) << self.pos_twid_scalar_shift;
        for i in 0..num2 as usize {
            let mut x = (-PI * (i as core::ffi::c_double + 0.25f64) / num1 as core::ffi::c_double) as f32;
            let mut r_tmp = (libm::cos(x as core::ffi::c_double) as f32
                * ((1 as core::ffi::c_longlong) << 31 as core::ffi::c_int) as f32)
                as core::ffi::c_longlong;
            let mut i_tmp = (libm::sin(x as core::ffi::c_double) as f32
                * ((1 as core::ffi::c_longlong) << 31 as core::ffi::c_int) as f32)
                as core::ffi::c_longlong;
            if r_tmp > 2147483647 as core::ffi::c_int as core::ffi::c_longlong {
                self.pre_twid[i].r = 2147483647 as core::ffi::c_int;
            } else if r_tmp < (-(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int) as core::ffi::c_longlong
            {
                self.pre_twid[i].r = -(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int;
            } else {
                self.pre_twid[i].r = r_tmp as core::ffi::c_int;
            }
            if i_tmp > 2147483647 as core::ffi::c_int as core::ffi::c_longlong {
                self.pre_twid[i].i = 2147483647 as core::ffi::c_int;
            } else if i_tmp < (-(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int) as core::ffi::c_longlong
            {
                self.pre_twid[i].i = -(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int;
            } else {
                self.pre_twid[i].i = i_tmp as core::ffi::c_int;
            }
            x = (-PI * i as core::ffi::c_double / num1 as core::ffi::c_double) as f32;
            r_tmp = ((libm::cos(x as core::ffi::c_double) * scale as core::ffi::c_double) as f32
                * pos_twid_scalar_shift_val as f32) as core::ffi::c_longlong;
            i_tmp = ((libm::sin(x as core::ffi::c_double) * scale as core::ffi::c_double) as f32
                * pos_twid_scalar_shift_val as f32) as core::ffi::c_longlong;
            if r_tmp > 2147483647 as core::ffi::c_int as core::ffi::c_longlong {
                self.pos_twid[i].r = 2147483647 as core::ffi::c_int;
            } else if r_tmp < (-(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int) as core::ffi::c_longlong
            {
                self.pos_twid[i].r = -(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int;
            } else {
                self.pos_twid[i].r = r_tmp as core::ffi::c_int;
            }
            if i_tmp > 2147483647 as core::ffi::c_int as core::ffi::c_longlong {
                self.pos_twid[i].i = 2147483647 as core::ffi::c_int;
            } else if i_tmp < (-(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int) as core::ffi::c_longlong
            {
                self.pos_twid[i].i = -(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int;
            } else {
                self.pos_twid[i].i = i_tmp as core::ffi::c_int;
            }
        }
        lhdc_enc_lossy_frequency_overlap(self.repeating, &mut self.freqency_win);
        self.pos_twid_scalar_shift = calc_pos_twid_scale_int_bits(self.num2);
        self.kiss_fft_cfg.reset(self.num2, 0);
    }

    pub fn new(max_frame_size: usize, max_frequency_repeating: usize, ch: usize) -> Self {
        Self {
            frequency_buffers: FrequencyBuffers::new(max_frame_size, max_frequency_repeating),
            pre_twid: copybox(Default::default(), max_frame_size / 2),
            pos_twid: copybox(Default::default(), max_frame_size / 2),
            freqency_win: copybox(0, max_frequency_repeating),
            frequency_process_ch_num: RepeatingChannels::new(max_frequency_repeating, ch),
            kiss_fft_cfg: State::new(max_frame_size),

            // TODO(b/454096420) To be populated in reset
            num1: 0,
            num2: 0,
            num3: 0,
            num4: 0,
            size: 0,
            repeating: 0,
            offset_num: 0,
            pos_twid_scalar_shift: 0,
        }
    }
}
fn calc_pos_twid_scale_int_bits(n4: core::ffi::c_int) -> core::ffi::c_int {
    if n4 <= 0 as core::ffi::c_int {
        return 0 as core::ffi::c_int;
    }
    let mut i: core::ffi::c_int = 0;
    let mut keep_positive_bits: core::ffi::c_int = 0 as core::ffi::c_int;
    while (1 as core::ffi::c_int) << i <= ((1 as core::ffi::c_int) << 15 as core::ffi::c_int) / n4 {
        keep_positive_bits += 1;
        i += 1;
    }
    31 as core::ffi::c_int - keep_positive_bits
}

pub fn lhdc_enc_lossy_frequency_operation(
    frequency: &mut Top,
    _c: core::ffi::c_int,
    output: &mut [i32],
) {
    let input =
        <[i32]>::mut_from_bytes(frequency.frequency_buffers.fft_coef_in.as_mut_bytes()).unwrap();

    let fft_coef_out: &mut [Complex] = &mut (frequency.frequency_buffers).fft_coef_out;
    let fft_iv: &mut [i32] = &mut (frequency.frequency_buffers).fft_iv;
    let scalar_shift = frequency.offset_num;
    let pos_twid_scalar_shift = frequency.pos_twid_scalar_shift;
    let num1 = frequency.num1 as usize;
    let num2 = frequency.num2 as usize;
    let num3 = frequency.num3 as usize;
    let num4 = frequency.num4 as usize;
    let freq_repeat = (frequency.frequency_process_ch_num).repeat_size_top();
    input.iter_mut().take(num1).for_each(|x| *x <<= scalar_shift);
    for i in 0..num4 {
        fft_iv[num3 + i] = -input[num1 - frequency.repeating as usize - 1 - i];
        fft_iv[num2 + i] = -input[num4 - 1 - i];
    }
    for i in 0..num3 {
        let fresh20 = input[num1 - num3 - 1 - i] as i64;
        let fresh21 = frequency.freqency_win[num3 + i] as i64;
        let fresh22 = input[num1 - num3 + i] as i64;
        let fresh23 = frequency.freqency_win[num3 - 1 - i] as i64;
        let fresh24 = &mut fft_iv[i];
        *fresh24 = ((-fresh20 * fresh21 - fresh22 * fresh23) >> 31) as i32;
        let fresh40 = freq_repeat[i] as i64;
        let fresh41 = frequency.freqency_win[i] as i64;
        let fresh42 = freq_repeat[frequency.repeating as usize - 1 - i] as i64;
        let fresh43 = frequency.freqency_win[frequency.repeating as usize - 1 - i] as i64;
        let fresh44 = &mut fft_iv[num2 + num4 + i];
        *fresh44 = ((fresh40 * fresh41 - fresh42 * fresh43) >> 31) as i32;
    }
    for i in 0..frequency.repeating as usize {
        freq_repeat[i] = input[num1 - frequency.repeating as usize + i];
    }

    let fft_coef_in: &mut [Complex] = &mut (frequency.frequency_buffers).fft_coef_in;

    for i in 0..num2 {
        let pdr_tmp_0 = fft_iv[i * 2] as i64;
        let pdi_tmp_0 = fft_iv[num1 - 1 - i * 2] as i64;
        let freq_pre_twid_r_tmp_0 = frequency.pre_twid[i].r as i64;
        let freq_pre_twid_i_tmp_0 = frequency.pre_twid[i].i as i64;
        fft_coef_in[i].r =
            ((pdr_tmp_0 * freq_pre_twid_r_tmp_0 - pdi_tmp_0 * freq_pre_twid_i_tmp_0) >> 31) as i32;
        fft_coef_in[i].i =
            ((pdi_tmp_0 * freq_pre_twid_r_tmp_0 + pdr_tmp_0 * freq_pre_twid_i_tmp_0) >> 31) as i32;
    }

    lhdc_fft(&mut frequency.kiss_fft_cfg, fft_coef_in, fft_coef_out);

    for i in 0..num2 {
        let cout_r = fft_coef_out[i].r as i64;
        let cout_i = fft_coef_out[i].i as i64;
        let freq_pos_r = frequency.pos_twid[i].r as i64;
        let freq_pos_i = frequency.pos_twid[i].i as i64;
        output[2 * i] = ((((cout_r * freq_pos_r - cout_i * freq_pos_i) >> scalar_shift)
            * num2 as i64)
            >> pos_twid_scalar_shift) as i32;
        output[num1 - 1 - 2 * i] =
            ((((-(cout_i * freq_pos_r + cout_r * freq_pos_i)) >> scalar_shift) * num2 as i64)
                >> pos_twid_scalar_shift) as i32;
    }
}
pub fn lhdc_enc_lossy_frequency_overlap(repeating: core::ffi::c_int, win_hann: &mut [i32]) {
    for (i, hann) in win_hann.iter_mut().take(repeating as usize).enumerate() {
        let sin_tmp =
            libm::sin(0.5f64 * PI * (i as core::ffi::c_double + 0.5f64) / repeating as core::ffi::c_double);
        let win_data = libm::sin(0.5f64 * PI * sin_tmp * sin_tmp) as f32;
        let win_tmp =
            (win_data * ((1 as core::ffi::c_longlong) << 31 as core::ffi::c_int) as f32) as core::ffi::c_longlong;
        if win_tmp > 2147483647 as core::ffi::c_int as core::ffi::c_longlong {
            *hann = 2147483647 as core::ffi::c_int;
        } else if win_tmp < (-(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int) as core::ffi::c_longlong {
            *hann = -(2147483647 as core::ffi::c_int) - 1 as core::ffi::c_int;
        } else {
            *hann = win_tmp as core::ffi::c_int;
        }
    }
}
