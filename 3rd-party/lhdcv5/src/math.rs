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

pub fn power_of_2(x: f32) -> f32 {
    let offset = if x < 0.0 { 1.0 } else { 0.0 };
    let clip = if x < -126.0 { -126.0 } else { x };
    let w = clip as i32;
    let z = clip - w as f32 + offset;
    let bits = ((1u32 << 23) as f32
        * (clip + 121.274_055_f32 + 27.728_024_f32 / (4.842_525_5_f32 - z) - 1.490_129_1_f32 * z))
        as u32;
    f32::from_bits(bits)
}

pub fn sqrt(x: f32) -> f32 {
    let threehalfs = 1.5f32;
    let x2 = x * 0.5f32;
    let mut y = x;
    let mut i = y.to_bits();
    i = 0x5f3759df - (i >> 1);
    y = f32::from_bits(i);
    y = y * (threehalfs - x2 * y * y);
    1.0f32 / y
}

/// Uses inverse multiplication to quickly divide by N.
///
/// N is provided as a const generic to encourage users to only use this approach for fixed N, as
/// computing `inverse` at runtime would be strictly slower than just performing the division.
///
/// This is an approximation - for example, for N=3, this is (x - 1) / 3 for positive x, and (x -
/// 2) / 3 for negative x.
///
/// Once we have non-bit-identical tests, we could consider replacing this with normal division.
pub fn div<const N: i32>(x: i32) -> i32 {
    let inverse: i64 = (i32::MAX as i64 + 1) / N as i64;
    ((x as i64 * inverse) >> 31) as i32
}

/// Computes `x / y`, rounding towards the nearest integer.
pub fn div_round(x: i32, y: i32) -> i32 {
    (x + (y / 2)).div_euclid(y)
}

/// Computes log-base-2 of the input, rounded.
///
/// Once we have non-bit-identical tests, we could consider replacing this with `.ilog2()`.
pub fn log_2(val: u32) -> u8 {
    let val = val as usize;
    if val == 0 {
        0
    } else if val <= 0xff {
        LOG2_256_TABLE[val] + 1
    } else if val <= 0xffff {
        LOG2_256_TABLE[val >> 8] + 9
    } else if val <= 0xffffff {
        LOG2_256_TABLE[val >> 16] + 17
    } else {
        LOG2_256_TABLE[val >> 24] + 25
    }
}

static LOG2_256_TABLE: [u8; 256] = [
    0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
];
