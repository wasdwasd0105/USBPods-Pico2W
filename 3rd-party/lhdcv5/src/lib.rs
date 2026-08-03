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

// usbpods no_std port: runs on the RP2350 (thumbv8m.main-none-eabihf) linked
// into the pico-sdk firmware. Heap = newlib malloc/free via src/rt.rs; log
// macros are compiled out (Cargo.toml max_level_off); float transcendentals
// (twiddle/window init only) come from libm.
#![no_std]

extern crate alloc;

pub mod common {
    pub mod lhdc_level;
}

mod kiss_fft;
pub mod lhdc_enc {
    pub mod lhdc_enc_freq_process;
    pub mod lhdc_enc_header;
    pub mod lhdc_enc_workspace;
}

pub mod lhdc_api;

mod arith;
mod bits;
pub mod enc;
mod ffi;
mod math;
mod rt;

/// Logging is compiled out on the embedded target (log max_level_off).
pub fn init_logging() {}
