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
use super::cirbuf::*;
use super::*;
use crate::enc::context::Context;
use crate::enc::process::*;
use crate::lhdc_enc::lhdc_enc_header::*;
use thiserror::Error;
use zerocopy::IntoBytes;

use log::{debug, error, info, warn};

fn copybox<T: Copy>(v: T, count: usize) -> Box<[T]> {
    core::iter::repeat_n(v, count).collect()
}

const MAX_QUEUE_FRAMES: usize = 4;
const MAX_ENC_FRAME_SAMPLE: usize = 960;
type SampleFrequency = u32;
const LHDC_ENC_IN_SR_192000HZ: SampleFrequency = 192000;
const LHDC_ENC_IN_SR_96000HZ: SampleFrequency = 96000;
const LHDC_ENC_IN_SR_48000HZ: SampleFrequency = 48000;
const LHDC_ENC_IN_SR_44100HZ: SampleFrequency = 44100;
type SampleFormat = u32;
const LHDC_ENC_IN_SMPL_FMT_S24: SampleFormat = 24;
const LHDC_ENC_IN_SMPL_FMT_S16: SampleFormat = 16;
type SampleFrame = u32;
const LHDC_ENC_IN_SAMPLE_FRAME_5MS_192000KHZ: SampleFrame = 960;
const LHDC_ENC_IN_SAMPLE_FRAME_5MS_96000KHZ: SampleFrame = 480;
const LHDC_ENC_IN_SAMPLE_FRAME_5MS_48000KHZ: SampleFrame = 240;
const LHDC_ENC_IN_SAMPLE_FRAME_5MS_44100KHZ: SampleFrame = 240;
type FrameDuration = u32;
const LHDC_ENC_IN_FRAME_1S: FrameDuration = 10000;
const LHDC_ENC_IN_FRAME_5MS: FrameDuration = 50;
type Interval = u32;
const LHDC_ENC_IN_INTERVAL_20MS: Interval = 20;
const LHDC_ENC_IN_INTERVAL_10MS: Interval = 10;
type Quality = u32;
const LHDC_ENC_IN_QUALITY_AUTO: Quality = 13;
const LHDC_ENC_IN_QUALITY_MAX_BITRATE: Quality = 12;
const LHDC_ENC_IN_QUALITY_LOW: Quality = 5;
const LHDC_ENC_IN_QUALITY_LOW0: Quality = 0;
type MtuSize = u32;
const LHDC_ENC_IN_MTU_MAX: MtuSize = 8192;
const LHDC_ENC_IN_MTU_3MBPS: MtuSize = 1023;
const LHDC_ENC_IN_MTU_2MBPS: MtuSize = 660;
const LHDC_ENC_IN_MTU_MIN: MtuSize = 300;
type Version = u32;
const LHDC_ENC_IN_VERSION_1: Version = 1;
type ErrorCode = i32;
const LHDC_ENC_IN_FRET_CODEC_NOT_READY: ErrorCode = -8;
const LHDC_ENC_IN_FRET_SUCCESS: ErrorCode = 0;

#[derive(Debug, Error)]
pub enum Error {
    #[error("Buffer too small")]
    BufTooSmall,
    #[error("Codec not ready")]
    CodecNotReady,
    #[error("Invalid codec")]
    InvalidCodec,
    #[error("Invalid Input Param")]
    InvalidInputParam,
    #[error("InvalidVersion: {0}")]
    InvalidVersion(u32),
    #[error(transparent)]
    Context(#[from] crate::enc::context::Error),
    #[error(transparent)]
    Process(#[from] crate::enc::process::Error),
}

pub type Result<T> = core::result::Result<T, Error>;

pub struct Parameters {
    pub version: u32,
    pub sample_rate: u32,
    pub bits_per_sample: u32,
    pub bits_per_sample_ui: u32,
    pub last_bitrate: u32,
    pub quality_status: u32,
    pub actual_bitrate: u32,
    pub max_bitrate_inx: u32,
    pub min_bitrate_inx: u32,
    pub samples_per_frame: u32,
    pub frame_duration: u32,
    pub max_frame_per_packet: u32,
    pub frame_per_packet: u32,
    pub host_mtu_size: u32,
    pub target_mtu_size: u32,
    pub encode_interval: u32,
    pub encoded_frame_size: u32,
    pub max_frame_per_interval: u32,
    pub update_frame_info: bool,
    pub lhdc_enc: Context,
    pub input_cbuf: CircularBuffer,
    pub enc_in_buf: Box<[i32]>,
    pub enc_out_buf: Box<[i32]>,
    // TODO(b/454096420) privatize through API
    pub bitrate_table: &'static [u32],
}

impl Parameters {
    pub fn new(version: u32) -> Result<Self> {
        // Stereo
        let ch_num = 2;
        if version != LHDC_ENC_IN_VERSION_1 {
            error!("Invalid version ({version})!");
            return Err(Error::InvalidVersion(version));
        }
        let mut lhdc_enc = Context::new(ch_num, 192000, 50, LHDC_ENC_MODE_OPTION_0)?;
        lhdc_enc.set_version(version as usize)?;
        let last_bitrate = g_bitrate_table_44k[LHDC_ENC_IN_QUALITY_LOW as usize] as u32;

        Ok(Self {
            version,
            sample_rate: LHDC_ENC_IN_SR_48000HZ,
            bits_per_sample: LHDC_ENC_IN_SMPL_FMT_S24,
            quality_status: LHDC_ENC_IN_QUALITY_LOW,
            last_bitrate,
            actual_bitrate: last_bitrate * 1000,
            max_bitrate_inx: LHDC_ENC_IN_QUALITY_MAX_BITRATE,
            min_bitrate_inx: LHDC_ENC_IN_QUALITY_LOW0,
            frame_duration: LHDC_ENC_IN_FRAME_5MS,
            host_mtu_size: LHDC_ENC_IN_MTU_2MBPS,
            encode_interval: LHDC_ENC_IN_INTERVAL_20MS,
            lhdc_enc,
            input_cbuf: CircularBuffer::new(MAX_QUEUE_FRAMES * MAX_ENC_FRAME_SAMPLE * ch_num),
            enc_in_buf: copybox(0, MAX_ENC_FRAME_SAMPLE * ch_num),
            enc_out_buf: copybox(0, MAX_ENC_FRAME_SAMPLE * ch_num),
            // Intended to be initialized later, were default zero initialized in original codebase
            bitrate_table: &g_bitrate_table_44k,
            bits_per_sample_ui: 0,
            encoded_frame_size: 0,
            frame_per_packet: 0,
            max_frame_per_packet: 0,
            samples_per_frame: 0,
            target_mtu_size: 0,
            update_frame_info: false,
            max_frame_per_interval: 0,
        })
    }
}

pub const LHDC_ENC_MODE_OPTION_0: Mode = 0;
pub type Mode = u32;

fn lhdcv5_encoder_cal_frame_size_and_frames_in_packet(handle: &mut Parameters) -> i32 {
    if handle.bits_per_sample_ui != LHDC_ENC_IN_SMPL_FMT_S16
        && handle.bits_per_sample_ui != LHDC_ENC_IN_SMPL_FMT_S24
    {
        error!("Invalid bits per sample ({})!", handle.bits_per_sample_ui);
        return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
    }
    if handle.frame_duration == LHDC_ENC_IN_FRAME_5MS {
        match handle.sample_rate {
            44100 => {
                if handle.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_44100KHZ {
                    error!("Invalid samples per frame ({})!", handle.samples_per_frame);
                    return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
                }
            }
            48000 => {
                if handle.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_48000KHZ {
                    error!("Invalid samples per frame ({})!", handle.samples_per_frame);
                    return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
                }
            }
            96000 => {
                if handle.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_96000KHZ {
                    error!("Invalid samples per frame ({})!", handle.samples_per_frame);
                    return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
                }
            }
            192000 => {
                if handle.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_192000KHZ {
                    error!("Invalid samples per frame ({})!", handle.samples_per_frame);
                    return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
                }
            }
            _ => {
                error!("Invalid samples per frame ({})!", handle.samples_per_frame);
                return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
            }
        }
    } else {
        error!("Invalid frame duration ({})!", handle.frame_duration);
        return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
    }
    if handle.encode_interval != LHDC_ENC_IN_INTERVAL_10MS
        && handle.encode_interval != LHDC_ENC_IN_INTERVAL_20MS
    {
        error!("Invalid encode interval ({})!", handle.encode_interval);
        return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
    }
    if handle.host_mtu_size < LHDC_ENC_IN_MTU_MIN || handle.host_mtu_size > LHDC_ENC_IN_MTU_MAX {
        error!("Invalid AVDTP MTU ({})!", handle.host_mtu_size);
        return LHDC_ENC_IN_FRET_CODEC_NOT_READY;
    }
    let bytes_per_sample: u32 = handle.bits_per_sample_ui.wrapping_div(8).wrapping_mul(2);
    let bytes_per_frame: u32 = handle.samples_per_frame.wrapping_mul(bytes_per_sample);
    debug!(
        "sampleRate({}) samples_per_frame({}) bits_per_sample({}) => bytes_per_pcm_frame({})",
        handle.sample_rate, handle.samples_per_frame, handle.bits_per_sample_ui, bytes_per_frame
    );
    let target_bytes_per_second: u32 = handle.actual_bitrate.wrapping_div(8);
    debug!(
        "target_bitrate({} kbps) actual_bitrate({})",
        handle.last_bitrate, handle.actual_bitrate,
    );
    let bytes_per_second: u32 = handle.sample_rate.wrapping_mul(bytes_per_sample);
    let compress_rate: f32 = target_bytes_per_second as f32 / bytes_per_second as f32;
    let max_output_bytes_per_frame: f32 = bytes_per_frame as f32 * compress_rate;
    let bytes_per_tick: u32 = handle
        .sample_rate
        .wrapping_mul(bytes_per_sample)
        .wrapping_mul(handle.encode_interval)
        .wrapping_div(1000);
    let frames_per_tick: u32 = (bytes_per_tick as f32 / bytes_per_frame as f32 + 0.5f32) as u32;
    debug!(
        "pcm_frames_per_tick({}) encode_interval_ms({}) frame_duration({})",
        frames_per_tick, handle.encode_interval, handle.frame_duration,
    );
    let max_output_bytes_per_tick: u32 =
        (max_output_bytes_per_frame * frames_per_tick as f32) as u32;
    let mut packet_per_tick: u32 = max_output_bytes_per_tick.wrapping_div(handle.host_mtu_size);
    packet_per_tick = if packet_per_tick <= 0 { 1 } else { packet_per_tick };
    handle.frame_per_packet = frames_per_tick.wrapping_div(packet_per_tick);
    handle.max_frame_per_packet = handle.frame_per_packet;
    let max_mtu_limit: u32 = handle.host_mtu_size;
    loop {
        if max_output_bytes_per_frame * handle.frame_per_packet as f32 > max_mtu_limit as f32 {
            handle.frame_per_packet = handle.frame_per_packet.wrapping_sub(1);
        } else {
            if (max_output_bytes_per_frame * handle.frame_per_packet as f32)
                < handle.host_mtu_size as f32
            {
                handle.target_mtu_size = max_output_bytes_per_frame as u32;
            } else {
                handle.target_mtu_size = handle.host_mtu_size.wrapping_div(handle.frame_per_packet);
            }
            break;
        }
    }
    debug!(
        "mtu({}), encoded_frame_size({}) => encoded_frame_per_packet({})",
        handle.host_mtu_size, handle.encoded_frame_size, handle.frame_per_packet,
    );
    LHDC_ENC_IN_FRET_SUCCESS
}

fn read_i32_bits(input: &mut &[u8], bytes: usize) -> i32 {
    let smear = (4 - bytes) * 8;
    let mut raw = [0; 4];
    raw[..bytes].copy_from_slice(&input[..bytes]);
    *input = &input[bytes..];
    i32::from_le_bytes(raw) << smear >> smear
}

fn lhdcv5_encoder_deinterleave24(mut in_0: &[u8], out: &mut [i32], out_samples: usize) {
    let left: usize = 0;
    let right: usize = out_samples;
    for i in 0..out_samples {
        out[left + i] = read_i32_bits(&mut in_0, 3);
        out[right + i] = read_i32_bits(&mut in_0, 3);
    }
}

fn lhdcv5_encoder_deinterleave16(mut in_0: &[u8], out: &mut [i32], out_samples: usize) {
    let left: usize = 0;
    let right: usize = out_samples;
    for i in 0..out_samples {
        out[left + i] = read_i32_bits(&mut in_0, 2);
        out[right + i] = read_i32_bits(&mut in_0, 2);
    }
}

impl Parameters {
    pub fn init(
        &mut self,
        sampling_freq: u32,
        bits_per_sample: u32,
        bitrate_inx: u32,
        frame_duration: u32,
        mtu: u32,
        interval: u32,
    ) -> Result<()> {
        let ch_num: u32 = 2;
        let mut samples_per_frame: i32 = 0;
        let mut encoded_frame_size: i32 = 0;
        let mut tmp_bitrate_inx: u32 = 0;
        if self.version != LHDC_ENC_IN_VERSION_1 {
            error!("Invalid version ({})!", self.version);
            return Err(Error::InvalidCodec);
        }
        if sampling_freq != LHDC_ENC_IN_SR_44100HZ
            && sampling_freq != LHDC_ENC_IN_SR_48000HZ
            && sampling_freq != LHDC_ENC_IN_SR_96000HZ
            && sampling_freq != LHDC_ENC_IN_SR_192000HZ
        {
            error!("Invalid sampling frequency ({sampling_freq})");
            return Err(Error::InvalidInputParam);
        }
        if bits_per_sample != LHDC_ENC_IN_SMPL_FMT_S16
            && bits_per_sample != LHDC_ENC_IN_SMPL_FMT_S24
        {
            error!("Invalid bits per sample ({bits_per_sample})!");
            return Err(Error::InvalidInputParam);
        }
        if bitrate_inx < LHDC_ENC_IN_QUALITY_LOW0 || bitrate_inx > LHDC_ENC_IN_QUALITY_AUTO {
            error!("Invalid bit rate (index) ({bitrate_inx})");
            return Err(Error::InvalidInputParam);
        }
        if frame_duration != 25 && frame_duration != LHDC_ENC_IN_FRAME_5MS {
            error!("Invalid frame duration ({frame_duration})!");
            return Err(Error::InvalidInputParam);
        }
        if mtu < LHDC_ENC_IN_MTU_MIN || mtu > LHDC_ENC_IN_MTU_MAX {
            error!("Invalid MTU ({mtu})");
            return Err(Error::InvalidInputParam);
        }
        if interval != LHDC_ENC_IN_INTERVAL_10MS && interval != LHDC_ENC_IN_INTERVAL_20MS {
            error!("Invalid encode interval ({interval})!");
            return Err(Error::InvalidInputParam);
        }
        if self.min_bitrate_inx < LHDC_ENC_IN_QUALITY_LOW0
            || self.min_bitrate_inx > LHDC_ENC_IN_QUALITY_LOW
        {
            error!("Error, min bit rate (index) ({})", self.min_bitrate_inx);
            return Err(Error::CodecNotReady);
        }
        if self.max_bitrate_inx < LHDC_ENC_IN_QUALITY_LOW
            || self.max_bitrate_inx > LHDC_ENC_IN_QUALITY_MAX_BITRATE
        {
            error!("Error, max bit rate (index) ({})", self.max_bitrate_inx);
            return Err(Error::CodecNotReady);
        }
        if self.min_bitrate_inx > self.max_bitrate_inx {
            error!(
                "Error, min and max bit rate (index) ({}) ({})",
                self.min_bitrate_inx, self.max_bitrate_inx,
            );
            return Err(Error::CodecNotReady);
        }
        self.bitrate_table = match sampling_freq {
            44100 => &g_bitrate_table_44k,
            96000 => &g_bitrate_table_96k,
            192000 => &g_bitrate_table_192k,
            _ => &g_bitrate_table_48k,
        };
        self.quality_status = bitrate_inx;
        if bitrate_inx >= LHDC_ENC_IN_QUALITY_LOW0 && bitrate_inx < LHDC_ENC_IN_QUALITY_AUTO {
            tmp_bitrate_inx = bitrate_inx;
            tmp_bitrate_inx =
                core::cmp::max(tmp_bitrate_inx as i32, self.min_bitrate_inx as i32) as u32;
            tmp_bitrate_inx =
                core::cmp::min(tmp_bitrate_inx as i32, self.max_bitrate_inx as i32) as u32;
            self.last_bitrate = self.bitrate_table[tmp_bitrate_inx as usize] as u32;
            self.quality_status = tmp_bitrate_inx;
        } else if bitrate_inx == LHDC_ENC_IN_QUALITY_AUTO {
            tmp_bitrate_inx = LHDC_ENC_IN_QUALITY_LOW;
            tmp_bitrate_inx =
                core::cmp::max(tmp_bitrate_inx as i32, self.min_bitrate_inx as i32) as u32;
            tmp_bitrate_inx =
                core::cmp::min(tmp_bitrate_inx as i32, self.max_bitrate_inx as i32) as u32;
            self.last_bitrate = self.bitrate_table[tmp_bitrate_inx as usize] as u32;
        }
        info!("target bitrate[{}]:{}", tmp_bitrate_inx, self.last_bitrate);
        self.sample_rate = sampling_freq;
        self.bits_per_sample_ui = bits_per_sample;
        self.bits_per_sample = bits_per_sample;
        self.frame_duration = frame_duration;
        self.host_mtu_size = mtu;
        self.encode_interval = interval;
        self.max_frame_per_interval = interval.wrapping_mul(10).wrapping_div(frame_duration);
        self.input_cbuf.reset();
        lhdc_enc_init(
            ch_num as i32,
            self.bits_per_sample as i32,
            sampling_freq as i32,
            frame_duration as i32,
            self.last_bitrate.wrapping_mul(1000) as core::ffi::c_int,
            &mut self.lhdc_enc,
        )?;
        lhdc_enc_get_samples_per_frame(&mut samples_per_frame, &mut self.lhdc_enc);
        self.samples_per_frame = samples_per_frame as _;
        if self.frame_duration == LHDC_ENC_IN_FRAME_5MS {
            match self.sample_rate {
                44100 => {
                    if self.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_44100KHZ {
                        error!("Invalid samples per frame ({})!", self.samples_per_frame);
                        return Err(Error::CodecNotReady);
                    }
                }
                48000 => {
                    if self.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_48000KHZ {
                        error!("Invalid samples per frame ({})!", self.samples_per_frame);
                        return Err(Error::CodecNotReady);
                    }
                }
                96000 => {
                    if self.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_96000KHZ {
                        error!("Invalid samples per frame ({})!", self.samples_per_frame);
                        return Err(Error::CodecNotReady);
                    }
                }
                192000 => {
                    if self.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_192000KHZ {
                        error!("Invalid samples per frame ({})!", self.samples_per_frame);
                        return Err(Error::CodecNotReady);
                    }
                }
                _ => {
                    error!("Invalid sample rate ({})!", self.sample_rate);
                    return Err(Error::CodecNotReady);
                }
            }
        } else {
            error!("Invalid frame duration ({})!", self.frame_duration);
            return Err(Error::CodecNotReady);
        }
        lhdc_enc_get_encoded_frame_size(&mut encoded_frame_size, &mut self.lhdc_enc);
        self.encoded_frame_size = encoded_frame_size as _;
        self.actual_bitrate = self
            .encoded_frame_size
            .wrapping_mul(8)
            .wrapping_mul(LHDC_ENC_IN_FRAME_1S)
            .wrapping_div(self.frame_duration);
        if self.sample_rate == LHDC_ENC_IN_SR_44100HZ {
            self.actual_bitrate = (self.actual_bitrate as f32 * 0.91875f64 as f32) as u32;
        }
        self.update_frame_info = false;
        lhdcv5_encoder_cal_frame_size_and_frames_in_packet(self);
        info!("Init Encoder: [sampleRate={} bit_per_sample={} bitrate={} samples_per_frame={} mtu={} encoded_frame_size={} encode_interval={} frame_duration={} frame_per_packet={} max_frame_per_interval={}",
            self.sample_rate,
            self.bits_per_sample,
            self.last_bitrate,
            self.samples_per_frame,
            self.host_mtu_size,
            self.encoded_frame_size,
            self.encode_interval,
            self.frame_duration,
            self.frame_per_packet,
            self.max_frame_per_interval,
        );
        Ok(())
    }
}
pub fn lhdcv5_encoder_get_bitrate(bitrate_inx: u32, bitrate_table: &[u32]) -> Result<u32> {
    if bitrate_inx >= bitrate_table.len() as _ {
        error!("Input bit rate (index) is out of range ({})!", bitrate_inx);
        return Err(Error::InvalidInputParam);
    }
    Ok(bitrate_table[bitrate_inx as usize])
}

pub fn lhdcv5_encoder_get_bitrate_inx(bitrate: u32, bitrate_table: &[u32]) -> Result<u32> {
    let mut index: u32 = 0;
    if bitrate > bitrate_table[bitrate_table.len() - 1] {
        return Err(Error::InvalidInputParam);
    }
    while index < bitrate_table.len() as _ {
        if bitrate_table[index as usize] >= bitrate {
            break;
        }
        index = index.wrapping_add(1);
    }
    Ok(index)
}

pub fn lhdcv5_encoder_set_target_bitrate_inx(
    lhdc: &mut Parameters,
    bitrate_inx: u32,
) -> Result<()> {
    if bitrate_inx < LHDC_ENC_IN_QUALITY_LOW0 || bitrate_inx > LHDC_ENC_IN_QUALITY_MAX_BITRATE {
        error!("Input parameter is invalid ({bitrate_inx})!");
        return Err(Error::InvalidInputParam);
    }
    let last_bitrate_inx = lhdcv5_encoder_get_bitrate_inx(lhdc.last_bitrate, lhdc.bitrate_table)?;
    if bitrate_inx != last_bitrate_inx {
        lhdc.last_bitrate = lhdcv5_encoder_get_bitrate(bitrate_inx, lhdc.bitrate_table)?;
        lhdc.update_frame_info = true;
    }
    info!("set target bitrate succeed (index:{}, bitrate:{})!", bitrate_inx, lhdc.last_bitrate);
    Ok(())
}

pub fn lhdcv5_encoder_set_max_bitrate_inx(
    lhdc: &mut Parameters,
    max_bitrate_inx: u32,
) -> Result<()> {
    if max_bitrate_inx < LHDC_ENC_IN_QUALITY_LOW
        || max_bitrate_inx > LHDC_ENC_IN_QUALITY_MAX_BITRATE
    {
        error!("Input MAX. bit rate (index) is invalid ({max_bitrate_inx})");
        return Err(Error::InvalidInputParam);
    }
    if max_bitrate_inx != lhdc.max_bitrate_inx {
        lhdc.max_bitrate_inx = max_bitrate_inx;
        if lhdc.quality_status < LHDC_ENC_IN_QUALITY_AUTO
            && lhdc.quality_status > lhdc.max_bitrate_inx
        {
            // savitech: for downgrade target bitrate limited by max bitrate
            lhdc.quality_status = lhdc.max_bitrate_inx;
        }
        let upd_max_bitrate = lhdcv5_encoder_get_bitrate(lhdc.max_bitrate_inx, lhdc.bitrate_table)?;
        info!(
            "set_max_bitrate: current bitrate ({}) vs. upd_max_bitrate ({})",
            lhdc.last_bitrate, upd_max_bitrate,
        );
        if lhdc.last_bitrate > upd_max_bitrate {
            // savitech: for downgrade target bitrate limited by max bitrate
            let max_bitrate_inx = lhdc.max_bitrate_inx;
            lhdcv5_encoder_set_target_bitrate_inx(lhdc, max_bitrate_inx)?;
        }
    }
    Ok(())
}

pub fn lhdcv5_encoder_set_min_bitrate_inx(
    lhdc: &mut Parameters,
    min_bitrate_inx: u32,
) -> Result<()> {
    if min_bitrate_inx < LHDC_ENC_IN_QUALITY_LOW0 || min_bitrate_inx > LHDC_ENC_IN_QUALITY_LOW {
        error!("Error, min bit rate(index) ({min_bitrate_inx})");
        return Err(Error::InvalidInputParam);
    }
    if min_bitrate_inx != lhdc.min_bitrate_inx {
        lhdc.min_bitrate_inx = min_bitrate_inx;
        if lhdc.quality_status < LHDC_ENC_IN_QUALITY_AUTO
            && lhdc.quality_status < lhdc.min_bitrate_inx
        {
            lhdc.quality_status = lhdc.min_bitrate_inx;
        }
        let upd_min_bitrate = lhdcv5_encoder_get_bitrate(lhdc.min_bitrate_inx, lhdc.bitrate_table)?;
        info!(
            "set_min_bitrate: current bitrate ({}) vs. upd_min_bitrate ({})",
            lhdc.last_bitrate, upd_min_bitrate,
        );
        if lhdc.last_bitrate < upd_min_bitrate {
            let min_bitrate_inx = lhdc.min_bitrate_inx;
            lhdcv5_encoder_set_target_bitrate_inx(lhdc, min_bitrate_inx)?;
        }
    }
    Ok(())
}

pub fn lhdcv5_encoder_get_frame_len(lhdc: &Parameters) -> Result<u32> {
    if lhdc.frame_duration == LHDC_ENC_IN_FRAME_5MS {
        match lhdc.sample_rate {
            44100 => {
                if lhdc.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_44100KHZ {
                    error!("Invalid samples per frame ({})!", lhdc.samples_per_frame);
                    return Err(Error::CodecNotReady);
                }
            }
            48000 => {
                if lhdc.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_48000KHZ {
                    error!("Invalid samples per frame ({})!", lhdc.samples_per_frame);
                    return Err(Error::CodecNotReady);
                }
            }
            96000 => {
                if lhdc.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_96000KHZ {
                    error!("Invalid samples per frame ({})!", lhdc.samples_per_frame);
                    return Err(Error::CodecNotReady);
                }
            }
            192000 => {
                if lhdc.samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_192000KHZ {
                    error!("Invalid samples per frame ({})!", lhdc.samples_per_frame);
                    return Err(Error::CodecNotReady);
                }
            }
            _ => {
                error!("Invalid sample rate ({})!", lhdc.sample_rate);
                return Err(Error::CodecNotReady);
            }
        }
    } else {
        error!("Invalid frame duration ({})!", lhdc.frame_duration);
        return Err(Error::CodecNotReady);
    }
    Ok(lhdc.samples_per_frame)
}

pub fn lhdcv5_encoder_encode(
    lhdc: &mut Parameters,
    in_0: &[u8],
    out: &mut [u8],
    written_bytes: &mut u32,
    out_frames: &mut u32,
) -> Result<()> {
    let ch_num: u32 = 2;
    let mut out_frames_cnt: u32 = 0;
    let mut encoded_bytes: i32 = 0;
    let mut encoded_frame_size: i32 = 0;
    *out_frames = 0;
    *written_bytes = 0;
    if lhdc.version != LHDC_ENC_IN_VERSION_1 {
        error!("Invalid version ({})!", lhdc.version);
        return Err(Error::InvalidCodec);
    }
    let samples_per_frame = lhdc.samples_per_frame;
    if lhdc.frame_duration == LHDC_ENC_IN_FRAME_5MS {
        match lhdc.sample_rate {
            44100 => {
                if samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_44100KHZ {
                    error!("Invalid samples per frame ({})!", samples_per_frame);
                    return Err(Error::CodecNotReady);
                }
            }
            48000 => {
                if samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_48000KHZ {
                    error!("Invalid samples per frame ({samples_per_frame})!");
                    return Err(Error::CodecNotReady);
                }
            }
            96000 => {
                if samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_96000KHZ {
                    error!("Invalid samples per frame ({samples_per_frame})!");
                    return Err(Error::CodecNotReady);
                }
            }
            192000 => {
                if samples_per_frame != LHDC_ENC_IN_SAMPLE_FRAME_5MS_192000KHZ {
                    error!("Invalid samples per frame ({samples_per_frame})!");
                    return Err(Error::CodecNotReady);
                }
            }
            _ => {
                error!("Invalid sample rate ({})!", lhdc.sample_rate);
                return Err(Error::CodecNotReady);
            }
        }
    } else {
        error!("Invalid frame duration ({})!", lhdc.frame_duration);
        return Err(Error::CodecNotReady);
    }
    let mut cbuf = &mut lhdc.input_cbuf;
    if lhdc.update_frame_info {
        lhdc.update_frame_info = false;
        let mut predict_to_update_new_bitrate = 0;
        if cbuf.is_empty() {
            predict_to_update_new_bitrate = 1;
            debug!("LossyOnly - predict: empty buffer!");
        }
        lhdc.update_frame_info = predict_to_update_new_bitrate != 1;
        if predict_to_update_new_bitrate == 1 {
            lhdc_enc_set_bitrate(
                (lhdc.last_bitrate).wrapping_mul(1000) as core::ffi::c_int,
                &mut lhdc.lhdc_enc,
            )?;
            lhdc_enc_get_encoded_frame_size(&mut encoded_frame_size, &mut lhdc.lhdc_enc);
            lhdc.encoded_frame_size = encoded_frame_size as u32;
            lhdc.actual_bitrate = (lhdc.encoded_frame_size)
                .wrapping_mul(8)
                .wrapping_mul(LHDC_ENC_IN_FRAME_1S)
                .wrapping_div(lhdc.frame_duration);
            if lhdc.sample_rate == LHDC_ENC_IN_SR_44100HZ {
                lhdc.actual_bitrate = (lhdc.actual_bitrate as f32 * 0.91875f64 as f32) as u32;
            }
            lhdcv5_encoder_cal_frame_size_and_frames_in_packet(&mut *lhdc);
            cbuf = &mut lhdc.input_cbuf;
            debug!(
                "Update bitrate:({}) frame size:({}) mtu:({})",
                lhdc.last_bitrate, encoded_frame_size, lhdc.host_mtu_size,
            );
        }
    }
    let frame_per_packet = lhdc.max_frame_per_packet;
    if frame_per_packet <= 0 || frame_per_packet > lhdc.max_frame_per_interval {
        error!("Invalid number of frames per packet ({frame_per_packet})!");
        return Err(Error::CodecNotReady);
    }
    encoded_frame_size = lhdc.encoded_frame_size as i32;
    if encoded_frame_size <= 0 || encoded_frame_size >= LHDC_ENC_IN_MTU_3MBPS as i32 {
        error!("Invalid encoded frames bytes ({})!", encoded_frame_size);
        return Err(Error::CodecNotReady);
    }
    let bytes_per_frame_in =
        samples_per_frame.wrapping_mul(ch_num).wrapping_mul(lhdc.bits_per_sample).wrapping_div(8);
    let bytes_per_frame = (samples_per_frame.wrapping_mul(ch_num) as core::ffi::c_ulong)
        .wrapping_mul(::core::mem::size_of::<i32>() as core::ffi::c_ulong)
        as u32;
    if in_0.len() < bytes_per_frame_in as _
        || lhdc.enc_in_buf.len() * core::mem::size_of::<i32>() < bytes_per_frame as usize
    {
        error!(
            "Input buffer is not enough ({}) ({}) ({}) ({})!",
            in_0.len(),
            lhdc.enc_in_buf.len() * core::mem::size_of::<i32>(),
            bytes_per_frame_in,
            bytes_per_frame,
        );
        return Err(Error::BufTooSmall);
    }
    // TODO(b/454096420) Should probably make the buffer an `i32` buffer by default
    let in_tmp = &mut lhdc.enc_in_buf;
    if lhdc.bits_per_sample_ui == LHDC_ENC_IN_SMPL_FMT_S24 {
        lhdcv5_encoder_deinterleave24(in_0, in_tmp, samples_per_frame as usize);
    } else if lhdc.bits_per_sample_ui == LHDC_ENC_IN_SMPL_FMT_S16 {
        lhdcv5_encoder_deinterleave16(in_0, in_tmp, samples_per_frame as usize);
    } else {
        error!("Invalid bits per sample ({})!", lhdc.bits_per_sample_ui);
        return Err(Error::CodecNotReady);
    }
    let out_tmp = lhdc.enc_out_buf.as_mut_bytes();
    let mut enc_bytes_cnt = 0;
    lhdc_enc_encode(in_tmp, out_tmp, &mut encoded_bytes, &mut lhdc.lhdc_enc);
    debug!(
        "LossyOnly - Output (mtu:{} encode_byte:{} cbLen:{} fppkt:{})",
        lhdc.host_mtu_size,
        encoded_bytes,
        cbuf.len(),
        lhdc.frame_per_packet,
    );
    let item_cnt = cbuf.put(&out_tmp[..encoded_bytes as usize]);
    if item_cnt as u32 >= lhdc.frame_per_packet {
        out_frames_cnt = item_cnt as u32;
        enc_bytes_cnt = cbuf.len() as _;
        cbuf.get(&mut out[0..cbuf.len()]);
    }
    if encoded_bytes != encoded_frame_size {
        warn!(
            "encoded frame size check not match (expected:{} result {})!",
            encoded_frame_size, encoded_bytes,
        );
    }
    *written_bytes = enc_bytes_cnt;
    *out_frames = out_frames_cnt;
    debug!(
        "final: mtu:{} written_bytes:{} out_frames:{}",
        lhdc.host_mtu_size, *written_bytes, *out_frames,
    );
    Ok(())
}
