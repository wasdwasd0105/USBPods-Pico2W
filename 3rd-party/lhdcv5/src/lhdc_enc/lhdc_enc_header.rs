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

use crate::enc::context::Context;
use crate::lhdc_enc::lhdc_enc_workspace::*;
use zerocopy::IntoBytes;

#[derive(Copy, Clone, Debug, PartialEq)]
pub struct SegmentSettings {
    pub drity_bit_adding: f32,
    pub segment_num_inv: core::ffi::c_int,
    pub segment_scale_jump: core::ffi::c_int,
    pub segment_scale_level: core::ffi::c_int,
    pub codeing_step_0: core::ffi::c_int,
    pub segment_num: usize,
    pub arith_init_0: core::ffi::c_int,
    pub arith_init_1: core::ffi::c_int,
    pub segment_offset: [usize; 34],
    pub segment_scale: [core::ffi::c_int; 34],
}

impl core::default::Default for SegmentSettings {
    fn default() -> Self {
        Self {
            drity_bit_adding: 0.0,
            segment_num_inv: 0,
            segment_scale_jump: 0,
            segment_scale_level: 0,
            codeing_step_0: 0,
            segment_num: 0,
            arith_init_0: 0,
            arith_init_1: 0,
            segment_offset: [0; 34],
            segment_scale: [0; 34],
        }
    }
}

impl SegmentSettings {
    // TODO(b/454096420) use a proper Result
    pub fn init(&mut self, index: usize) -> bool {
        *self = match index {
            240 => SegmentSettings {
                drity_bit_adding: 0.01325,
                segment_num_inv: 67108864,
                segment_scale_jump: 27,
                segment_scale_level: 773094113,
                codeing_step_0: 80,
                segment_num: 32,
                arith_init_0: 57,
                arith_init_1: 63,
                segment_offset: [
                    0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 37, 43, 50, 58, 68, 79,
                    92, 107, 124, 144, 168, 195, 226, 263, 306, 355, 413, 480, 55,
                ],
                segment_scale: [
                    67108864, 67108864, 67108864, 67108864, 67108864, 67108864, 67108864, 67108864,
                    67108864, 67108864, 67108864, 67108864, 47396895, 66949894, 75655391, 89055067,
                    107823107, 133266164, 150594768, 193382512, 231135434, 282955815, 352663864,
                    423428306, 498423650, 625815217, 769924588, 911188351, 1107494116, 1372820649,
                    1638256461, 2003249672, 67851879, 95874153,
                ],
            },
            480 => SegmentSettings {
                drity_bit_adding: 0.01325,
                segment_num_inv: 67108864,
                segment_scale_jump: 27,
                segment_scale_level: 773094113,
                codeing_step_0: 80,
                segment_num: 32,
                arith_init_0: 57,
                arith_init_1: 63,
                segment_offset: [
                    0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 37, 43, 50, 58, 68, 79,
                    92, 107, 124, 144, 168, 195, 226, 263, 306, 355, 413, 480, 11452,
                ],
                segment_scale: [
                    67108864, 67108864, 67108864, 67108864, 67108864, 67108864, 67108864, 67108864,
                    67108864, 67108864, 67108864, 67108864, 47396895, 66949894, 75655391, 89055067,
                    107823107, 133266164, 150594768, 193382512, 231135434, 282955815, 352663864,
                    423428306, 498423650, 625815217, 769924588, 911188351, 1107494116, 1372820649,
                    1638256461, 2003249672, 63587498, 521458953,
                ],
            },
            481 => SegmentSettings {
                drity_bit_adding: 0.01325,
                segment_num_inv: 67108864,
                segment_scale_jump: 28,
                segment_scale_level: 773094113,
                codeing_step_0: 80,
                segment_num: 32,
                arith_init_0: 57,
                arith_init_1: 63,
                segment_offset: [
                    0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 42, 48, 56, 64, 74,
                    85, 98, 112, 130, 149, 172, 198, 228, 262, 302, 348, 400, 58548,
                ],
                segment_scale: [
                    134217728, 134217728, 134217728, 134217728, 134217728, 134217728, 134217728,
                    134217728, 134217728, 134217728, 134217728, 134217728, 89491108, 119338311,
                    159140196, 141477908, 188663822, 188690438, 251622743, 268435456, 325422212,
                    367194803, 454686273, 471593025, 595780490, 656314565, 774223823, 894784853,
                    1052836570, 1193383111, 1383827787, 1632437396, 99871891, 2264894,
                ],
            },
            482 => SegmentSettings {
                drity_bit_adding: 0.01325,
                segment_num_inv: 89478485,
                segment_scale_jump: 29,
                segment_scale_level: 773094113,
                codeing_step_0: 80,
                segment_num: 24,
                arith_init_0: 57,
                arith_init_1: 63,
                segment_offset: [
                    0, 3, 6, 9, 12, 15, 18, 21, 24, 28, 33, 39, 45, 54, 63, 74, 87, 103, 121, 142,
                    167, 196, 231, 272, 320, 43, 50, 58, 68, 79, 92, 107, 480, 8919,
                ],
                segment_scale: [
                    178956971, 178956971, 178956971, 178956971, 178956971, 178956971, 178956971,
                    178956971, 178982217, 190941298, 212186927, 282955815, 251551763, 335449667,
                    365996716, 412977625, 447455542, 530392494, 606248364, 679093957, 780677884,
                    862584857, 981942410, 1118481067, 885853822, 781230438, 456652743, 368435756,
                    325455312, 567125203, 107812307, 732258164, 955596768, 89251519,
                ],
            },
            960 => SegmentSettings {
                drity_bit_adding: 0.09375,
                segment_num_inv: 67108864,
                segment_scale_jump: 28,
                segment_scale_level: 966367641,
                codeing_step_0: 160,
                segment_num: 32,
                arith_init_0: 96,
                arith_init_1: 64,
                segment_offset: [
                    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 56, 64, 74, 86, 100, 116, 136,
                    158, 184, 214, 248, 288, 336, 390, 452, 526, 612, 710, 826, 960, 1852,
                ],
                segment_scale: [
                    67108864, 67108864, 67108864, 67108864, 67108864, 67108864, 67108864, 67108864,
                    67108864, 67108864, 67108864, 67108864, 47396895, 66949894, 75655391, 89055067,
                    107823107, 133266164, 150594768, 193382512, 231135434, 282955815, 352663864,
                    423428306, 498423650, 625815217, 769924588, 911188351, 1107494116, 1372820649,
                    1638256461, 2003249672, 831835531, 894915673,
                ],
            },
            1920 => SegmentSettings {
                drity_bit_adding: 0.0625,
                segment_num_inv: 67108864,
                segment_scale_jump: 25,
                segment_scale_level: 966367641,
                codeing_step_0: 320,
                segment_num: 32,
                arith_init_0: 96,
                arith_init_1: 64,
                segment_offset: [
                    0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 112, 128, 148, 172, 200, 232,
                    272, 316, 368, 428, 496, 576, 672, 780, 904, 1052, 1224, 1420, 1652, 1920,
                    2582,
                ],
                segment_scale: [
                    62914560, 62914560, 62914560, 62914560, 62914560, 62914560, 62914560, 62914560,
                    62914560, 62914560, 62914560, 62914560, 44434589, 62765525, 70926928, 83489124,
                    101084162, 124937028, 141182594, 181296105, 216689469, 265271076, 330622372,
                    396964037, 467272171, 586701765, 721804300, 854239078, 1038275733, 1287019358,
                    1535865432, 1878046567, 968574163, 252697486,
                ],
            },
            _ => return false,
        };
        true
    }
}

pub type HeaderInfoIndex = core::ffi::c_uint;
pub const ALL_HEADER_INFO_NUM: HeaderInfoIndex = 6;
pub const META_INDEX: HeaderInfoIndex = 5;
pub const LARC_INDEX: HeaderInfoIndex = 4;
pub const AR_INDEX: HeaderInfoIndex = 3;
pub const JAS_INDEX: HeaderInfoIndex = 2;
pub const VERSION_INDEX: HeaderInfoIndex = 1;
pub const ENC_SIZE_INDEX: HeaderInfoIndex = 0;

#[derive(Copy, Clone, Default)]
pub struct Header {
    pub info: core::ffi::c_ushort,
    pub ext_data: [core::ffi::c_uchar; 10],
    pub enc_frm_len_provided: core::ffi::c_int,
    pub enc_frm_len_need_update: core::ffi::c_uchar,
    pub meta_data_loop_count: core::ffi::c_int,
}

impl Header {
    pub fn set_info(&mut self, index: HeaderInfoIndex, value: i32) {
        set_hdr_info(&mut self.info, index, value);
    }
    pub fn get_info(&self, index: HeaderInfoIndex) -> i32 {
        get_hdr_info(&self.info, index)
    }
}

static HEADER_INFO_MAX: [core::ffi::c_ushort; 6] = [0x3ff, 0xc00, 0x1000, 0x2000, 0x4000, 0x8000];
static HEADER_INFO_OFFSETS: [core::ffi::c_int; 6] = [0, 10, 12, 13, 14, 15];

#[inline]
fn set_hdr_info(hdr_info: &mut core::ffi::c_ushort, index: HeaderInfoIndex, value: core::ffi::c_int) {
    *hdr_info = (*hdr_info as core::ffi::c_int & !(HEADER_INFO_MAX[index as usize] as core::ffi::c_int)
        | (value << HEADER_INFO_OFFSETS[index as usize])) as core::ffi::c_ushort;
}

#[inline]
fn get_hdr_info(hdr_info: &u16, index: HeaderInfoIndex) -> core::ffi::c_int {
    (*hdr_info as core::ffi::c_int & HEADER_INFO_MAX[index as usize] as core::ffi::c_int)
        >> HEADER_INFO_OFFSETS[index as usize]
}

pub fn enc_process_header(
    hdr: &mut Header,
    _ch: i32,
    enc_frm_len_usable: &mut i32,
    encoded_frame: &mut [u8],
) -> core::ffi::c_int {
    let mut hdr_size: core::ffi::c_int = 0;
    *enc_frm_len_usable = hdr.enc_frm_len_provided;
    set_hdr_info(&mut hdr.info, ENC_SIZE_INDEX, *enc_frm_len_usable);
    encoded_frame[0] = hdr.info.as_bytes()[0];
    encoded_frame[1] = hdr.info.as_bytes()[1];
    hdr_size += 2 as core::ffi::c_int;
    hdr_size
}

pub fn lhdc_enc_get_encoded_frame_size(encoded_frame_size: &mut core::ffi::c_int, ctx: &mut Context) {
    let mut extra_bytes: core::ffi::c_int = 0;
    let enc_frm_len_provided = ctx.hdr_s().enc_frm_len_provided;
    let ecb: &mut fdata_all_buffer_struct = ctx.ebuffer();
    extra_bytes += 2 as core::ffi::c_int;
    *encoded_frame_size = extra_bytes + ecb.ch_num * enc_frm_len_provided;
}
