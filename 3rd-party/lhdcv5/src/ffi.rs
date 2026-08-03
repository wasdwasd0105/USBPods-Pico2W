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
use crate::lhdc_api::*;
use core::mem::ManuallyDrop;
use core::slice::{from_raw_parts, from_raw_parts_mut};

#[allow(non_camel_case_types)]
pub type HANDLE_LHDC_BT = *const Context;

#[allow(non_camel_case_types)]
pub type STATUS_LHDC_BT = i32;

#[no_mangle]
pub extern "C" fn lhdcv5_enc_ffi_init() {
    crate::init_logging();
}

fn to_lhdc_fret<T>(res: Result<T>) -> STATUS_LHDC_BT {
    match res {
        Ok(_) => LHDC_FRET_SUCCESS,
        Err(Error::InvalidInputParam) => LHDC_FRET_INVALID_INPUT_PARAM,
        Err(Error::InvalidCodec) => LHDC_FRET_INVALID_CODEC,
        Err(Error::Internal(_)) => LHDC_FRET_ERROR,
    }
}

/// # Safety
///
/// `handle` must be a valid pointer.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_get_handle(
    version: u32,
    handle: *mut HANDLE_LHDC_BT,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    let cb = Box::new(Context::new(version).unwrap());
    // SAFETY: `handle` has been checked non-null.
    unsafe {
        *handle = Box::into_raw(cb);
    }
    0
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_free_handle(handle: HANDLE_LHDC_BT) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    // SAFETY: `handle` has been checked non-null; the caller is responsible for
    // invoking the method with a valid handle.
    let _ = unsafe { Box::from_raw(handle.cast_mut()) };
    0
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_init_encoder(
    handle: HANDLE_LHDC_BT,
    sampling_freq: u32,
    bits_per_sample: u32,
    bitrate_inx: u32,
    mtu: u32,
    interval: u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    let mut cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    to_lhdc_fret(cb.init_encoder(
        sampling_freq,
        bits_per_sample,
        bitrate_inx,
        LHDC_FRAME_5MS,
        mtu,
        interval,
    ))
}

/// Returns current quality status
///
/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`. `quality_status` must
/// be a valid pointer.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_get_quality_mode(
    handle: HANDLE_LHDC_BT,
    quality_status: *mut u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    assert!(!quality_status.is_null());
    let cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    // SAFETY: `quality_status` has been checked non-null.
    unsafe {
        *quality_status = cb.quality_status();
    }
    0
}

/// Returns current (last) bitrate
///
/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`. `bitrate` must
/// be a valid pointer.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_get_last_bitrate(
    handle: HANDLE_LHDC_BT,
    bitrate: *mut u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    assert!(!bitrate.is_null());
    let cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    // SAFETY: `bitrate` has been checked non-null.
    unsafe {
        *bitrate = cb.last_bitrate();
    }
    0
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`. `bitrate_inx` must
/// be a valid pointer.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_get_bitrate_index(
    handle: HANDLE_LHDC_BT,
    bitrate: u32,
    bitrate_inx: *mut u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    assert!(!bitrate_inx.is_null());
    let cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    let ret = cb.get_target_bitrate_inx(bitrate);
    if let Ok(inx) = ret {
        // SAFETY: `bitrate_inx` has been checked non-null.
        unsafe {
            *bitrate_inx = inx;
        }
    }
    to_lhdc_fret(ret)
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_set_bitrate_index(
    handle: HANDLE_LHDC_BT,
    bitrate_inx: u32,
    upd_qual_status: bool,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    let mut cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    to_lhdc_fret(cb.set_target_bitrate_inx(bitrate_inx, upd_qual_status))
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_set_max_bitrate(
    handle: HANDLE_LHDC_BT,
    max_bitrate_inx: u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    let mut cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    to_lhdc_fret(cb.set_max_bitrate_inx(max_bitrate_inx))
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_set_min_bitrate(
    handle: HANDLE_LHDC_BT,
    min_bitrate_inx: u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    let mut cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    to_lhdc_fret(cb.set_min_bitrate_inx(min_bitrate_inx))
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`. `samples_per_frame` must
/// be a valid pointer.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_get_block_size(
    handle: HANDLE_LHDC_BT,
    samples_per_frame: *mut u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    assert!(!samples_per_frame.is_null());
    let cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    let ret = cb.get_block_size();
    if let Ok(block_size) = ret {
        // SAFETY: `samples_per_frame` has been checked non-null.
        unsafe {
            *samples_per_frame = block_size;
        }
    }
    to_lhdc_fret(ret)
}

/// # Safety
///
/// `handle` must be a value previously returned by `lhdcv5_enc_ffi_get_handle`,
/// and not yet released by `lhdcv5_enc_ffi_free_handle`. `written_bytes` and
/// `written_frames` must be valid pointers.
/// `in_buf` must be non-null and point to a memory zone of size at least `in_pcm_len`.
/// `out_buf` must be non-null and point to a memory zone of size at least `out_buf_len`.
#[no_mangle]
pub unsafe extern "C" fn lhdcv5_enc_ffi_encode(
    handle: HANDLE_LHDC_BT,
    in_pcm: *const u8,
    in_pcm_len: usize,
    out_buf: *mut u8,
    out_buf_len: usize,
    written_bytes: *mut u32,
    written_frames: *mut u32,
) -> STATUS_LHDC_BT {
    assert!(!handle.is_null());
    assert!(!in_pcm.is_null());
    assert!(!out_buf.is_null());
    assert!(!written_bytes.is_null());
    assert!(!written_frames.is_null());
    let mut cb = ManuallyDrop::new(
        // SAFETY: `handle` has been checked non-null; the caller is responsible for
        // invoking the method with a valid handle.
        unsafe { Box::from_raw(handle.cast_mut()) },
    );
    to_lhdc_fret(cb.enc_process(
        // SAFETY: `in_pcm` has been checked non-null; the caller is responsible
        // for ensuring it points to a memory zone of sufficient size.
        unsafe { from_raw_parts(in_pcm, in_pcm_len) },
        // SAFETY: `out_buf` has been checked non-null; the caller is responsible
        // for ensuring it points to a memory zone of sufficient size.
        unsafe { from_raw_parts_mut(out_buf, out_buf_len) },
        // SAFETY: `written_bytes` has been checked non-null.
        unsafe { written_bytes.as_mut().unwrap() },
        // SAFETY: `written_frames` has been checked non-null.
        unsafe { written_frames.as_mut().unwrap() },
    ))
}
