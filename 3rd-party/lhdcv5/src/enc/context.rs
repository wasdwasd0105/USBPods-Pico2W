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
use crate::lhdc_api::lhdc_api_internal::LHDC_ENC_MODE_OPTION_0;
use crate::lhdc_enc::lhdc_enc_header::{Header, VERSION_INDEX};
use crate::lhdc_enc::lhdc_enc_workspace::fdata_all_buffer_struct;
use thiserror::Error;

pub struct Context {
    pub khz_max: usize,
    pub ms_max: usize,
    pub max_frame_size: usize,
    pub max_lossy_umem8_len: usize,
    pub max_frequency_repeating: usize,
    pub hdr_s: Header,
    pub ebuffer: fdata_all_buffer_struct,
    pub enc_arith_s: arith::EncoderBase,
    pub encoded_data_ch0: [u32; 171],
    pub encoded_data_ch1: [u32; 171],
}

#[derive(Error, Debug)]
pub enum Error {
    #[error("Bad configuration value")]
    BadConfig,
    #[error("Bad version {0}")]
    BadVersion(usize),
}

pub type Result<T> = core::result::Result<T, Error>;

impl Context {
    pub fn khz_max(&self) -> i32 {
        self.khz_max as i32
    }
    pub fn ms_max(&self) -> i32 {
        self.ms_max as i32
    }
    pub fn max_frame_size(&self) -> i32 {
        self.max_frame_size as i32
    }
    pub fn max_lossy_umem8_len(&self) -> i32 {
        self.max_lossy_umem8_len as i32
    }
    pub fn max_freq_repeating(&self) -> i32 {
        self.max_frequency_repeating as i32
    }
    pub fn hdr_s(&mut self) -> &mut Header {
        &mut self.hdr_s
    }
    pub fn ebuffer(&mut self) -> &mut fdata_all_buffer_struct {
        &mut self.ebuffer
    }
    pub fn encoded_data_ch0(&mut self) -> &mut [u32; 171] {
        &mut self.encoded_data_ch0
    }
    pub fn encoded_data_ch1(&mut self) -> &mut [u32; 171] {
        &mut self.encoded_data_ch1
    }
    pub fn new(ch: usize, mut khz_max: usize, ms_max: usize, mode: u32) -> Result<Self> {
        if !(1..=8).contains(&ch)
            || khz_max != 8000
                && khz_max != 16000
                && khz_max != 24000
                && khz_max != 32000
                && khz_max != 44100
                && khz_max != 48000
                && khz_max != 96000
                && khz_max != 192000
            || ms_max != 50
            || (mode != LHDC_ENC_MODE_OPTION_0)
        {
            return Err(Error::BadConfig);
        }

        if khz_max == 44100 {
            khz_max = 48000;
        }
        let max_frame_size = khz_max * ms_max / 10000;
        let max_frame_size_10ms = max_frame_size * 10;

        let mult = match ms_max {
            50 => 20,
            _ => panic!("Bad ms_max (should be impossible) {ms_max}"),
        };
        let max_frequency_repeating = khz_max * mult / 10_000;
        let max_lossy_umem8_len =
            2 * (if max_frame_size_10ms > 960 { max_frame_size_10ms } else { 960 });

        Ok(Self {
            khz_max,
            ms_max,
            max_frame_size,
            max_lossy_umem8_len,
            max_frequency_repeating,
            hdr_s: Default::default(),
            ebuffer: fdata_all_buffer_struct::new(
                khz_max as i32,
                ms_max as i32,
                max_frame_size as i32,
                max_lossy_umem8_len as i32,
                max_frequency_repeating as i32,
            ),
            enc_arith_s: arith::EncoderBase::default(),
            encoded_data_ch0: [0; 171],
            encoded_data_ch1: [0; 171],
        })
    }
    pub fn set_version(&mut self, version: usize) -> Result<()> {
        if version > 3 {
            Err(Error::BadVersion(version))
        } else {
            self.hdr_s().set_info(VERSION_INDEX, version as i32);
            Ok(())
        }
    }
}
