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

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]
use alloc::boxed::Box;
use alloc::vec::Vec;
#[allow(unused_imports)]
use alloc::vec;
pub mod cirbuf;
pub mod lhdc_api_internal;

use lhdc_api_internal::*;
use log::{error, info};

pub type lhdc_log_level = core::ffi::c_uint;
pub const LHDC_LOGMGR_LEVEL_DEBUG_NO_LOG: lhdc_log_level = 256;
pub const LHDC_LOGMGR_LEVEL_MAX: lhdc_log_level = 135;
pub const LHDC_LOGMGR_LEVEL_DEBUG_INTERNAL: lhdc_log_level = 128;
pub const LHDC_LOGMGR_LEVEL_DEBUG: lhdc_log_level = 7;
pub const LHDC_LOGMGR_LEVEL_INFO: lhdc_log_level = 6;
pub const LHDC_LOGMGR_LEVEL_NOTICE: lhdc_log_level = 5;
pub const LHDC_LOGMGR_LEVEL_WARNING: lhdc_log_level = 4;
pub const LHDC_LOGMGR_LEVEL_ERROR: lhdc_log_level = 3;
pub const LHDC_LOGMGR_LEVEL_CRIT: lhdc_log_level = 2;
pub const LHDC_LOGMGR_LEVEL_ALERT: lhdc_log_level = 1;
pub const LHDC_LOGMGR_LEVEL_EMERG: lhdc_log_level = 0;
pub type __LHDC_SAMPLE_FREQ__ = core::ffi::c_uint;
pub const LHDC_SR_192000HZ: __LHDC_SAMPLE_FREQ__ = 192000;
pub const LHDC_SR_96000HZ: __LHDC_SAMPLE_FREQ__ = 96000;
pub const LHDC_SR_48000HZ: __LHDC_SAMPLE_FREQ__ = 48000;
pub const LHDC_SR_44100HZ: __LHDC_SAMPLE_FREQ__ = 44100;
pub type __LHDCBT_SMPL_FMT__ = core::ffi::c_uint;
pub const LHDCBT_SMPL_FMT_S24: __LHDCBT_SMPL_FMT__ = 24;
pub const LHDCBT_SMPL_FMT_S16: __LHDCBT_SMPL_FMT__ = 16;
pub type __LHDC_FRAME_DURATION__ = core::ffi::c_uint;
pub const LHDC_FRAME_5MS: __LHDC_FRAME_DURATION__ = 50;
pub type __LHDC_ENC_INTERVAL__ = core::ffi::c_uint;
pub const LHDC_ENC_INTERVAL_20MS: __LHDC_ENC_INTERVAL__ = 20;
pub const LHDC_ENC_INTERVAL_10MS: __LHDC_ENC_INTERVAL__ = 10;
pub type __LHDC_QUALITY__ = core::ffi::c_uint;
pub const LHDC_QUALITY_INVALID: __LHDC_QUALITY__ = 130;
pub const LHDC_QUALITY_CTRL_END: __LHDC_QUALITY__ = 129;
pub const LHDC_QUALITY_CTRL_RESET_ABR: __LHDC_QUALITY__ = 128;
pub const LHDC_QUALITY_UNLIMIT: __LHDC_QUALITY__ = 14;
pub const LHDC_QUALITY_AUTO: __LHDC_QUALITY__ = 13;
pub const LHDC_QUALITY_MAX_BITRATE: __LHDC_QUALITY__ = 12;
pub const LHDC_QUALITY_HIGH5: __LHDC_QUALITY__ = 12;
pub const LHDC_QUALITY_HIGH4: __LHDC_QUALITY__ = 11;
pub const LHDC_QUALITY_HIGH3: __LHDC_QUALITY__ = 10;
pub const LHDC_QUALITY_HIGH2: __LHDC_QUALITY__ = 9;
pub const LHDC_QUALITY_HIGH1: __LHDC_QUALITY__ = 8;
pub const LHDC_QUALITY_HIGH: __LHDC_QUALITY__ = 7;
pub const LHDC_QUALITY_MID: __LHDC_QUALITY__ = 6;
pub const LHDC_QUALITY_LOW: __LHDC_QUALITY__ = 5;
pub const LHDC_QUALITY_LOW4: __LHDC_QUALITY__ = 4;
pub const LHDC_QUALITY_LOW3: __LHDC_QUALITY__ = 3;
pub const LHDC_QUALITY_LOW2: __LHDC_QUALITY__ = 2;
pub const LHDC_QUALITY_LOW1: __LHDC_QUALITY__ = 1;
pub const LHDC_QUALITY_LOW0: __LHDC_QUALITY__ = 0;
pub type __LHDC_MTU_SIZE__ = core::ffi::c_uint;
pub const LHDC_MTU_MAX: __LHDC_MTU_SIZE__ = 8192;
pub const LHDC_MTU_MHDT_8DH5: __LHDC_MTU_SIZE__ = 2820;
pub const LHDC_MTU_MHDT_6DH5: __LHDC_MTU_SIZE__ = 2089;
pub const LHDC_MTU_MHDT_4DH5: __LHDC_MTU_SIZE__ = 1392;
pub const LHDC_MTU_3MBPS: __LHDC_MTU_SIZE__ = 1023;
pub const LHDC_MTU_2MBPS: __LHDC_MTU_SIZE__ = 660;
pub const LHDC_MTU_MIN: __LHDC_MTU_SIZE__ = 300;
pub type __LHDC_VERSION__ = core::ffi::c_uint;
pub const LHDC_VERSION_INVALID: __LHDC_VERSION__ = 2;
pub const LHDC_VERSION_1: __LHDC_VERSION__ = 1;
pub type __LHDC_ENC_TYPE__ = core::ffi::c_uint;
pub const LHDC_ENC_TYPE_INVALID: __LHDC_ENC_TYPE__ = 2;
pub const LHDC_ENC_TYPE_LHDC: __LHDC_ENC_TYPE__ = 1;
pub const LHDC_ENC_TYPE_UNKNOWN: __LHDC_ENC_TYPE__ = 0;
pub type LHDC_ENC_TYPE_T = __LHDC_ENC_TYPE__;
pub type __LHDC_LOG_LEVEL__ = core::ffi::c_uint;
pub const LHDC_LOG_LEVEL_DEBUG: __LHDC_LOG_LEVEL__ = 7;
pub const LHDC_LOG_LEVEL_INFO: __LHDC_LOG_LEVEL__ = 6;
pub const LHDC_LOG_LEVEL_NOTICE: __LHDC_LOG_LEVEL__ = 5;
pub const LHDC_LOG_LEVEL_WARNING: __LHDC_LOG_LEVEL__ = 4;
pub const LHDC_LOG_LEVEL_ERROR: __LHDC_LOG_LEVEL__ = 3;
pub const LHDC_LOG_LEVEL_CRIT: __LHDC_LOG_LEVEL__ = 2;
pub const LHDC_LOG_LEVEL_ALERT: __LHDC_LOG_LEVEL__ = 1;
pub const LHDC_LOG_LEVEL_EMERG: __LHDC_LOG_LEVEL__ = 0;
pub const LHDC_FRET_BUF_NOT_ENOUGH: i32 = -11;
pub const LHDC_FRET_ERROR: i32 = -10;
pub const LHDC_FRET_AR_NOT_READY: i32 = -9;
pub const LHDC_FRET_CODEC_NOT_READY: i32 = -8;
pub const LHDC_FRET_INVALID_CODEC: i32 = -7;
pub const LHDC_FRET_INVALID_HANDLE_AR: i32 = -6;
pub const LHDC_FRET_INVALID_HANDLE_CBUF: i32 = -5;
pub const LHDC_FRET_INVALID_HANDLE_ENC: i32 = -4;
pub const LHDC_FRET_INVALID_HANDLE_PARA: i32 = -3;
pub const LHDC_FRET_INVALID_HANDLE_CB: i32 = -2;
pub const LHDC_FRET_INVALID_INPUT_PARAM: i32 = -1;
pub const LHDC_FRET_SUCCESS: i32 = 0;
pub type HANDLE_LHDC_BT = Box<Context>;

pub struct Context {
    enc_type: u32,
    #[allow(dead_code)]
    err: i32,
    enc: Parameters,
}

pub static g_bitrate_table_44k: [u32; 15] =
    [64, 160, 192, 240, 320, 400, 480, 900, 1000, 1100, 1200, 1300, 1400, 99999, 1536000];
pub static g_bitrate_table_48k: [u32; 15] =
    [64, 160, 192, 256, 320, 400, 500, 900, 1000, 1100, 1200, 1300, 1400, 99999, 1536000];
pub static g_bitrate_table_96k: [u32; 15] =
    [64, 160, 192, 256, 320, 400, 500, 900, 1000, 1100, 1200, 1300, 1400, 99999, 1536000];
pub static g_bitrate_table_192k: [u32; 15] =
    [64, 160, 192, 256, 320, 400, 500, 900, 1000, 1100, 1200, 1300, 1400, 99999, 1536000];

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("Invalid input param")]
    InvalidInputParam,
    #[error("Invalid codec")]
    InvalidCodec,
    #[error(transparent)]
    Internal(#[from] lhdc_api_internal::Error),
}

pub type Result<T> = core::result::Result<T, Error>;

impl Context {
    pub fn new(version: u32) -> Result<Self> {
        Ok(Self { enc_type: LHDC_ENC_TYPE_LHDC, err: 0, enc: Parameters::new(version)? })
    }

    pub fn get_target_bitrate_inx(&self, bitrate_kbps: u32) -> Result<u32> {
        let bitrate_table = &self.enc.bitrate_table;
        if bitrate_kbps > bitrate_table[bitrate_table.len() - 1] {
            return Err(Error::InvalidInputParam);
        }
        Ok(lhdcv5_encoder_get_bitrate_inx(bitrate_kbps, bitrate_table)?)
    }

    pub fn last_bitrate(&self) -> u32 {
        self.enc.last_bitrate
    }

    pub fn quality_status(&self) -> u32 {
        self.enc.quality_status
    }

    pub fn set_target_bitrate_inx(
        &mut self,
        bitrate_inx: u32,
        upd_qual_status: bool,
    ) -> Result<u32> {
        if bitrate_inx < LHDC_QUALITY_LOW0 || bitrate_inx > LHDC_QUALITY_AUTO {
            error!("Input bit rate (index) is invalid ({})!!!", bitrate_inx);
            return Err(Error::InvalidInputParam);
        }
        let mut upd_bitrate_inx = bitrate_inx;
        match self.enc_type {
            1 => {
                if bitrate_inx == LHDC_QUALITY_AUTO {
                    upd_bitrate_inx =
                        LHDC_QUALITY_LOW.clamp(self.enc.min_bitrate_inx, self.enc.max_bitrate_inx);
                } else {
                    upd_bitrate_inx =
                        upd_bitrate_inx.clamp(self.enc.min_bitrate_inx, self.enc.max_bitrate_inx);
                }
                if upd_qual_status {
                    if bitrate_inx == LHDC_QUALITY_AUTO {
                        self.enc.quality_status = LHDC_QUALITY_AUTO;
                    } else {
                        self.enc.quality_status = upd_bitrate_inx;
                    }
                }
                lhdcv5_encoder_set_target_bitrate_inx(&mut self.enc, upd_bitrate_inx)?;
            }
            _ => {
                error!("Invalid encode type ({})!", self.enc_type);
                return Err(Error::InvalidCodec);
            }
        }
        info!(
            "set target quality succeed: quality_index:{} bitrate_inx:{}",
            self.enc.quality_status, upd_bitrate_inx,
        );

        Ok(upd_bitrate_inx)
    }

    pub fn set_max_bitrate_inx(&mut self, max_bitrate_inx: u32) -> Result<u32> {
        if max_bitrate_inx < LHDC_QUALITY_LOW || max_bitrate_inx > LHDC_QUALITY_MAX_BITRATE {
            error!("Input MAX. bit rate (index) is invalid ({})!", max_bitrate_inx);
            return Err(Error::InvalidInputParam);
        }
        match self.enc_type {
            1 => {
                lhdcv5_encoder_set_max_bitrate_inx(&mut self.enc, max_bitrate_inx)?;
                Ok(self.enc.max_bitrate_inx)
            }
            _ => {
                error!("Invalid encode type ({})!", self.enc_type);
                Err(Error::InvalidCodec)
            }
        }
    }

    pub fn set_min_bitrate_inx(&mut self, min_bitrate_inx: u32) -> Result<u32> {
        if min_bitrate_inx < LHDC_QUALITY_LOW0 || min_bitrate_inx > LHDC_QUALITY_LOW {
            error!("Input MIN. bit rate (index) is invalid ({})!", min_bitrate_inx);
            return Err(Error::InvalidInputParam);
        }
        match self.enc_type {
            1 => {
                lhdcv5_encoder_set_min_bitrate_inx(&mut self.enc, min_bitrate_inx)?;
                Ok(self.enc.min_bitrate_inx)
            }
            _ => {
                error!("Invalid encode type ({})!", self.enc_type);
                Err(Error::InvalidCodec)
            }
        }
    }

    pub fn init_encoder(
        &mut self,
        sampling_freq: u32,
        bits_per_sample: u32,
        bitrate_inx: u32,
        frame_duration: u32,
        mtu: u32,
        interval: u32,
    ) -> Result<()> {
        if sampling_freq != LHDC_SR_44100HZ
            && sampling_freq != LHDC_SR_48000HZ
            && sampling_freq != LHDC_SR_96000HZ
            && sampling_freq != LHDC_SR_192000HZ
        {
            error!("Invalid sampling frequency ({})!", sampling_freq);
            return Err(Error::InvalidInputParam);
        }
        if bits_per_sample != LHDCBT_SMPL_FMT_S16 && bits_per_sample != LHDCBT_SMPL_FMT_S24 {
            error!("Invalid bits per sample ({bits_per_sample})!");
            return Err(Error::InvalidInputParam);
        }
        if bitrate_inx < LHDC_QUALITY_LOW0 || bitrate_inx > LHDC_QUALITY_AUTO {
            error!("Invalid bit rate (index) ({bitrate_inx})!");
            return Err(Error::InvalidInputParam);
        }
        if frame_duration != LHDC_FRAME_5MS {
            error!("Invalid frame duration ({frame_duration})!");
            return Err(Error::InvalidInputParam);
        }
        if mtu < LHDC_MTU_MIN || mtu > LHDC_MTU_MAX {
            error!("Invalid MTU ({mtu})");
            return Err(Error::InvalidInputParam);
        }
        if interval != LHDC_ENC_INTERVAL_10MS && interval != LHDC_ENC_INTERVAL_20MS {
            error!("Invalid encode interval ({interval})!");
            return Err(Error::InvalidInputParam);
        }
        match self.enc_type {
            1 => {
                self.enc.init(
                    sampling_freq,
                    bits_per_sample,
                    bitrate_inx,
                    frame_duration,
                    mtu,
                    interval,
                )?;
                self.enc.frame_duration = frame_duration;
                let _ = lhdcv5_encoder_get_frame_len(&self.enc)?;
            }
            _ => {
                error!("Invalid encode type ({})!", self.enc_type);
                return Err(Error::InvalidCodec);
            }
        }
        info!("init encoder done [sample_rate:{} bits_per_sample:{} frame_duration:{} interval:{} bitrate_inx:{} mtu:{} lastBitrate:{} handle:{:?}]",
            sampling_freq,
            bits_per_sample,
            frame_duration,
            interval,
            bitrate_inx,
            mtu,
            self.enc.last_bitrate,
            self as *const _,
        );
        Ok(())
    }

    pub fn get_block_size(&self) -> Result<u32> {
        match self.enc_type {
            1 => {
                let block_size = lhdcv5_encoder_get_frame_len(&self.enc)?;
                Ok(block_size)
            }
            _ => {
                error!("Invalid encode type ({})!", self.enc_type);
                Err(Error::InvalidCodec)
            }
        }
    }

    pub fn enc_process(
        &mut self,
        pcm: &[u8],
        out: &mut [u8],
        written: &mut u32,
        out_frames: &mut u32,
    ) -> Result<()> {
        match self.enc_type {
            1 => {
                lhdcv5_encoder_encode(&mut self.enc, pcm, out, written, out_frames)?;
            }
            _ => {
                error!("Invalid encode type ({})!", self.enc_type);
                return Err(Error::InvalidCodec);
            }
        }
        Ok(())
    }
}
