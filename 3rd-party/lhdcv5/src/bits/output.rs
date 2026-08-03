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

use thiserror::Error;
use zerocopy::{FromBytes, IntoBytes};

pub struct BitOutputter<'a> {
    out: &'a mut [u32],
    write_head: usize,
    // Bits staged
    enc_r_d32_l: u32,
    // Bits valid
    enc_r_num: u32,
}

#[derive(Error, Debug)]
pub enum Error {
    #[error("Tried to write more bits than fit in the argument (>32)")]
    TooManyBits,
    #[error("Not enough space in output buffer")]
    OutBufOverflow,
}

pub type Result<T> = core::result::Result<T, Error>;

impl<'a> BitOutputter<'a> {
    pub fn zero_pad(&mut self) -> Result<()> {
        if self.enc_r_num & 7 != 0 {
            self.write_bits(0, 8)?;
        }
        Ok(())
    }
    fn push(&mut self, val: u32) {
        self.out[self.write_head] = val;
        self.write_head += 1;
    }
    fn full(&self) -> bool {
        self.write_head >= self.out.len()
    }
    pub fn new(out: &'a mut [u32]) -> Self {
        Self { out, write_head: 0, enc_r_d32_l: 0, enc_r_num: 0 }
    }
    pub fn write_bits(&mut self, mut data: u32, bit_len: u32) -> Result<()> {
        if bit_len > 32 {
            return Err(Error::TooManyBits);
        }
        if bit_len == 0 {
            return Ok(());
        }
        // TODO(b/454096420) review to see if we can avoid wrapping arith and sign casting
        let bit_len_l = 32u32.wrapping_sub(self.enc_r_num);
        let offset = bit_len.wrapping_sub(bit_len_l) as i32;
        if offset >= 0 && self.full() {
            return Err(Error::OutBufOverflow);
        }
        data &= 0xffffffffu32 >> (32 - bit_len);
        if offset >= 0 && self.enc_r_num != 0 {
            let mut mem_data = self.enc_r_d32_l << bit_len_l;
            mem_data |= data >> offset;
            self.push(
                ((mem_data & 0xff000000) >> 24)
                    | ((mem_data & 0xff0000) >> 8)
                    | ((mem_data & 0xff00) << 8)
                    | ((mem_data & 0xff) << 24),
            );
            self.enc_r_num = offset as u32;
            self.enc_r_d32_l = data;
        } else if offset >= 0 && self.enc_r_num == 0 {
            self.push(
                ((data & 0xff000000) >> 24)
                    | ((data & 0xff0000) >> 8)
                    | ((data & 0xff00) << 8)
                    | ((data & 0xff) << 24),
            );
        } else {
            self.enc_r_d32_l <<= bit_len;
            self.enc_r_d32_l |= data;
            self.enc_r_num = self.enc_r_num.wrapping_add(bit_len);
        }
        Ok(())
    }

    pub fn write_bytes(&mut self, data: &[u8]) -> Result<()> {
        let mut i: usize = 0;
        // If we still have data stored internally, feed byte by byte until it can be flushed as a
        // doubleword write.
        while self.enc_r_num != 0 && i < data.len() {
            self.write_bits(data[i] as u32, 8)?;
            i += 1;
        }
        // Then, feed doubleword at a time until we can't
        while i + 4 <= data.len() {
            if self.full() {
                return Err(Error::OutBufOverflow);
            }
            // TODO(b/454096420) should this be explicitly LE, or does it need to be native endian?
            // Original was native endian, so it's been preserved for now.
            let dw = *<u32 as FromBytes>::ref_from_prefix(&data[i..]).unwrap().0;
            self.push(dw);
            i += 4;
        }
        // Finally, feed the remaining data in
        while i < data.len() {
            self.write_bits(data[i] as u32, 8)?;
            i += 1;
        }
        Ok(())
    }
    pub fn clear(&mut self) -> Result<usize> {
        let umem8 = self.out[self.write_head..].as_mut_bytes();
        let mut tmp_bits = 0;
        if self.enc_r_num != 0 {
            tmp_bits = self.enc_r_num.div_ceil(8);
            if tmp_bits as usize > umem8.len() {
                return Err(Error::OutBufOverflow);
            }
            let n = self.enc_r_d32_l << (32 - self.enc_r_num);
            let x = n.as_bytes();
            // TODO(b/454096420) this is almost certainly another place where fixed endian should be used
            for i in 0..tmp_bits as usize {
                umem8[i] = x[3 - i];
            }
        };
        Ok(self.write_head * core::mem::size_of::<u32>() + (tmp_bits as usize))
    }
}
