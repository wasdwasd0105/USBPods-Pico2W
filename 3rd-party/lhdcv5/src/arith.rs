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

mod ring_buf;

use crate::bits::output::BitOutputter;
use ring_buf::RingBuf;
use thiserror::Error;

#[derive(Copy, Clone, Default)]
// TODO(b/454096420) name this better when we know what it is
struct SymbolData {
    symbol_num: u8,
    // First symbol appears never-touched?
    cdf: [u32; 6],
    input: [u32; 5],
    buf_3: [u32; 5],
}

impl SymbolData {
    fn load(&mut self, count: &[u32]) {
        self.input[..count.len()].copy_from_slice(count);
        self.symbol_num = count.len() as u8;
        debug_assert!(self.symbol_num >= 3);
        // Should be impossible to violate due to copy_from_slice
        debug_assert!(self.symbol_num <= 5);
    }
    fn calc(&mut self) {
        let symbol_num = self.symbol_num as usize;
        let mut sum = 0;
        for i in 0..symbol_num {
            sum += self.input[i];
            self.cdf[i + 1] = sum;
        }

        for i in 0..symbol_num {
            self.buf_3[i] = self.input[i] + (self.input[i] >> 5);
        }

        // Put sum of counts through scaler
        let weight = scale_of_probility(sum);
        for i in 0..symbol_num {
            self.cdf[i + 1] *= weight;
            self.cdf[i + 1] >>= 31 - 15;
        }
    }
}

pub struct Encoder<'a> {
    base: &'a mut EncoderBase,
    out: &'a mut [u8],
}

#[derive(Default)]
pub struct EncoderBase {
    // Logical digit
    big_digit: u32,
    // Bits set-in-stone, 0 means ready
    valid_bits: u32,
    write_head: usize,
    ring: RingBuf,
    arith: SymbolData,
}

const HIGH_BYTE_ONE: u32 = 0x01_00_00_00;

#[derive(Error, Debug)]
pub enum Error {
    #[error("Output buffer was not large enough")]
    OutBufOverflow,
    #[error(transparent)]
    BitOutputter(#[from] crate::bits::output::Error),
}

pub type Result<T> = core::result::Result<T, Error>;

impl<'a> Encoder<'a> {
    fn symbol_frequency(&mut self, c: u32) {
        let i = self.base.ring.peek() as usize;
        if self.base.arith.input[i] > 1 {
            self.base.arith.input[i] -= 1;
        }
        self.base.arith.input[c as usize] += 1;
        if self.base.arith.input[c as usize] >= self.base.arith.buf_3[c as usize] {
            self.base.arith.calc()
        }
        self.base.ring.push(c as u8);
    }
    pub fn new(base: &'a mut EncoderBase, out: &'a mut [u8]) -> Self {
        base.valid_bits = 0xffffffff;
        base.big_digit = 0;
        base.arith = Default::default();
        base.ring = Default::default();
        base.write_head = 0;

        Self { base, out }
    }
    /// True means the write went through, false means it failed
    fn write(&mut self, val: u8) -> Result<()> {
        if self.base.write_head >= self.out.len() {
            Err(Error::OutBufOverflow)
        } else {
            self.out[self.base.write_head] = val;
            self.base.write_head += 1;
            Ok(())
        }
    }
    fn add_with_overflow(&mut self, val: u32) {
        let (acc, overflow) = self.base.big_digit.overflowing_add(val);
        if overflow {
            let mut carry_idx = self.base.write_head - 1;
            while self.out[carry_idx] == u8::MAX {
                self.out[carry_idx] = 0;
                carry_idx -= 1;
            }
            self.out[carry_idx] += 1;
        }
        self.base.big_digit = acc;
    }
    fn flush(&mut self) -> Result<()> {
        while self.base.valid_bits < HIGH_BYTE_ONE {
            self.write((self.base.big_digit >> 24) as u8)?;
            self.base.valid_bits <<= 8;
            self.base.big_digit <<= 8;
        }
        Ok(())
    }

    fn symbol_1(&mut self, symbol: u32) -> Result<()> {
        // len will always only go from 1 to 0 once
        let len: u32 = self.base.valid_bits >> 15;
        // TODO(b/454096420) figure out why this multiplication is happening.
        // Not safety critical, but weird
        let lower = len.wrapping_mul(self.base.arith.cdf[symbol as usize]);
        self.add_with_overflow(lower);
        self.base.valid_bits =
            len.wrapping_mul(self.base.arith.cdf[(symbol + 1) as usize]).wrapping_sub(lower);
        self.flush()
    }

    fn symbol_2(&mut self, symbol: u32) {
        self.base.arith.input[symbol as usize] += 1;
        if self.base.arith.buf_3[symbol as usize] <= self.base.arith.input[symbol as usize] {
            self.base.arith.calc();
        }
    }

    pub fn program(&mut self, count: &[u32], shifting_num: i32) {
        self.base.arith.load(count);
        self.base.arith.calc();
        self.base.ring.reset(shifting_num);
    }

    pub fn symbol_3(&mut self, symbol: u32) -> Result<()> {
        self.symbol_1(symbol)?;
        if self.base.ring.shift(symbol as u8) {
            self.symbol_2(symbol)
        } else {
            self.symbol_frequency(symbol)
        }
        Ok(())
    }
    // TODO(b/454096420): This function is not well-tested by the current suite.
    pub fn clear(&mut self, bit_outputter: &mut BitOutputter) -> Result<i32> {
        if self.base.valid_bits <= 2 * HIGH_BYTE_ONE {
            self.add_with_overflow(HIGH_BYTE_ONE >> 1);
            self.base.valid_bits = HIGH_BYTE_ONE >> 9;
        } else {
            self.add_with_overflow(HIGH_BYTE_ONE);
            self.base.valid_bits = HIGH_BYTE_ONE >> 1;
        };
        self.flush()?;
        bit_outputter.write_bytes(&self.out[..self.base.write_head])?;
        Ok(self.base.write_head as i32)
    }
}

fn scale_of_probility(deno: u32) -> u32 {
    0x80000000 / deno
}

fn inverse_total(t: u32) -> u32 {
    0x10000 / t
}

fn get_esti_total(cnt_tbl: &[u32], est_count: &mut [u32]) -> u32 {
    est_count[..cnt_tbl.len()].copy_from_slice(cnt_tbl);
    cnt_tbl.iter().sum()
}

fn bit_esti(count: u32, inverse_all: u32) -> f32 {
    let vxf = count.wrapping_mul(inverse_all) as f32 * (1.0f32 / 65536.0f32);
    let mut y = vxf.to_bits() as f32;
    y *= 1.192_092_9e-7_f32;
    126.942_696_f32 - y
}

pub fn esti(udata8: &[u8], counting_table: &[u32]) -> i32 {
    let mut data_count: [u32; 5] = [0; 5];
    let mut all = get_esti_total(counting_table, &mut data_count);
    for elem in udata8 {
        data_count[*elem as usize] += 1;
    }
    all += udata8.len() as u32;
    let inverse_all = inverse_total(all);
    let mut est_sum = 0.0;
    for i in 0..counting_table.len() {
        let est_bits = bit_esti(data_count[i], inverse_all);
        est_sum += (data_count[i]).wrapping_sub(counting_table[i]) as f32 * est_bits;
    }
    est_sum as i32
}
