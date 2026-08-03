use alloc::boxed::Box;
use alloc::vec::Vec;
#[allow(unused_imports)]
use alloc::vec;
/*
Copyright (c) 2003-2010, Mark Borgerding

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the author nor the names of any contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

use crate::math;
use zerocopy::{FromBytes, Immutable, IntoBytes};

// TODO(b/454096420) there's a good chance that some of the uses of the AsBytes/IntoBytes
// are assuming that r is next to i and so it's form of an array.
// Check this once done by removing repr(C) and using the field order randomizer.
// If it's doing this, see if it can be converted to explicit .r/.i accesses
#[derive(Copy, Clone, Default, IntoBytes, Immutable, FromBytes)]
#[repr(C)]
pub struct Complex {
    pub r: i32,
    pub i: i32,
}

pub struct State {
    pub nfft: i32,
    pub inverse: i32,
    pub factors: [i32; 64],
    pub twiddles: Box<[Complex]>,
}

impl State {
    pub fn new(max_frame_size: usize) -> Self {
        Self {
            nfft: 0,
            inverse: 0,
            factors: [0; 64],
            twiddles: core::iter::repeat_n(Complex::default(), max_frame_size / 2).collect(),
        }
    }

    pub fn reset(&mut self, nfft: i32, inverse: i32) {
        self.nfft = nfft;
        self.inverse = inverse;
        self.resolve();
    }

    fn resolve(&mut self) {
        for i in 0..(self.nfft as usize) {
            let pi: f64 = core::f64::consts::PI;
            let mut phase: f64 = -2.0 * pi * i as f64 / self.nfft as f64;
            if self.inverse != 0 {
                phase *= -1_f64;
            }
            self.twiddles[i].r = libm::floor(0.5f64 + 2147483647_f64 * libm::cos(phase)) as i32;
            self.twiddles[i].i = libm::floor(0.5f64 + 2147483647_f64 * libm::sin(phase)) as i32;
        }
        kf_factor(self.nfft, &mut self.factors);
    }
}

fn kf_bfly2(f_out: &mut [Complex], fstride: usize, st: &mut State, mut m: i32) {
    let mut tw1: &mut [Complex] = &mut st.twiddles;
    let mut t: Complex = Complex { r: 0, i: 0 };
    let (mut f_out, mut f_out_2) = f_out.split_at_mut(m as usize);
    loop {
        let tw = tw1[0];
        f_out[0].r >>= 1;
        f_out[0].i >>= 1;
        f_out_2[0].r >>= 1;
        f_out_2[0].i >>= 1;
        t.r = ((f_out_2[0].r as i64 * tw.r as i64 - f_out_2[0].i as i64 * tw.i as i64
            + ((1) << (31 - 1)))
            >> 31) as i32;
        t.i = ((f_out_2[0].r as i64 * tw.i as i64
            + f_out_2[0].i as i64 * tw.r as i64
            + ((1) << (31 - 1)) as i64)
            >> 31) as i32;
        tw1 = &mut tw1[fstride..];
        f_out_2[0].r = f_out[0].r - t.r;
        f_out_2[0].i = f_out[0].i - t.i;
        f_out[0].r += t.r;
        f_out[0].i += t.i;
        f_out_2 = &mut f_out_2[1..];
        f_out = &mut f_out[1..];
        m -= 1;
        if m == 0 {
            break;
        }
    }
}
fn kf_bfly4(mut f_out: &mut [Complex], fstride: usize, st: &mut State, m: usize) {
    let mut scratch: [Complex; 6] = [Complex { r: 0, i: 0 }; 6];
    let mut k: usize = m;
    let m2: usize = 2usize.wrapping_mul(m);
    let m3: usize = 3usize.wrapping_mul(m);
    let tw = &mut st.twiddles;
    let mut twi1 = 0;
    let mut twi2 = 0;
    let mut twi3 = 0;
    loop {
        f_out[0].r >>= 2;
        f_out[0].i >>= 2;
        f_out[m].r >>= 2;
        f_out[m].i >>= 2;
        f_out[m2].r >>= 2;
        f_out[m2].i >>= 2;
        f_out[m3].r >>= 2;
        f_out[m3].i >>= 2;
        scratch[0].r = ((f_out[m].r as i64 * (tw[twi1]).r as i64
            - f_out[m].i as i64 * (tw[twi1]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[0].i = ((f_out[m].r as i64 * (tw[twi1]).i as i64
            + f_out[m].i as i64 * (tw[twi1]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[1].r = ((f_out[m2].r as i64 * (tw[twi2]).r as i64
            - f_out[m2].i as i64 * (tw[twi2]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[1].i = ((f_out[m2].r as i64 * (tw[twi2]).i as i64
            + f_out[m2].i as i64 * (tw[twi2]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[2].r = ((f_out[m3].r as i64 * (tw[twi3]).r as i64
            - f_out[m3].i as i64 * (tw[twi3]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[2].i = ((f_out[m3].r as i64 * (tw[twi3]).i as i64
            + f_out[m3].i as i64 * (tw[twi3]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[5].r = f_out[0].r - scratch[1].r;
        scratch[5].i = f_out[0].i - scratch[1].i;
        f_out[0].r += scratch[1].r;
        f_out[0].i += scratch[1].i;
        scratch[3].r = scratch[0].r + scratch[2].r;
        scratch[3].i = scratch[0].i + scratch[2].i;
        scratch[4].r = scratch[0].r - scratch[2].r;
        scratch[4].i = scratch[0].i - scratch[2].i;
        f_out[m2].r = f_out[0].r - scratch[3].r;
        f_out[m2].i = f_out[0].i - scratch[3].i;
        twi1 += fstride;
        twi2 += fstride * 2;
        twi3 += fstride * 3;
        f_out[0].r += scratch[3].r;
        f_out[0].i += scratch[3].i;
        if st.inverse != 0 {
            f_out[m].r = scratch[5].r - scratch[4].i;
            f_out[m].i = scratch[5].i + scratch[4].r;
            f_out[m3].r = scratch[5].r + scratch[4].i;
            f_out[m3].i = scratch[5].i - scratch[4].r;
        } else {
            f_out[m].r = scratch[5].r + scratch[4].i;
            f_out[m].i = scratch[5].i - scratch[4].r;
            f_out[m3].r = scratch[5].r - scratch[4].i;
            f_out[m3].i = scratch[5].i + scratch[4].r;
        }
        f_out = &mut f_out[1_usize..];
        k = k.wrapping_sub(1);
        if k == 0 {
            break;
        }
    }
}
fn kf_bfly3(mut f_out: &mut [Complex], fstride: usize, st: &mut State, m: usize) {
    let mut k = m;
    let m2 = 2 * m;
    let mut scratch: [Complex; 5] = [Complex { r: 0, i: 0 }; 5];
    let tw = &mut st.twiddles;
    let epi3 = tw[fstride * m];
    let mut twi1 = 0;
    let mut twi2 = 0;
    loop {
        f_out[0].r =
            ((f_out[0].r as i64 * (2147483647 / 3) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out[0].i =
            ((f_out[0].i as i64 * (2147483647 / 3) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out[m].r =
            ((f_out[m].r as i64 * (2147483647 / 3) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out[m].i =
            ((f_out[m].i as i64 * (2147483647 / 3) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out[m2].r =
            ((f_out[m2].r as i64 * (2147483647 / 3) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out[m2].i =
            ((f_out[m2].i as i64 * (2147483647 / 3) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[1].r = ((f_out[m].r as i64 * (tw[twi1]).r as i64
            - f_out[m].i as i64 * (tw[twi1]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[1].i = ((f_out[m].r as i64 * (tw[twi1]).i as i64
            + f_out[m].i as i64 * (tw[twi1]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[2].r = ((f_out[m2].r as i64 * (tw[twi2]).r as i64
            - f_out[m2].i as i64 * (tw[twi2]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[2].i = ((f_out[m2].r as i64 * (tw[twi2]).i as i64
            + f_out[m2].i as i64 * (tw[twi2]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[3].r = scratch[1].r + scratch[2].r;
        scratch[3].i = scratch[1].i + scratch[2].i;
        scratch[0].r = scratch[1].r - scratch[2].r;
        scratch[0].i = scratch[1].i - scratch[2].i;
        twi1 += fstride;
        twi2 += fstride * 2;
        f_out[m].r = f_out[0].r - (scratch[3].r >> 1);
        f_out[m].i = f_out[0].i - (scratch[3].i >> 1);
        scratch[0].r =
            ((scratch[0].r as i64 * epi3.i as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[0].i =
            ((scratch[0].i as i64 * epi3.i as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out[0].r += scratch[3].r;
        f_out[0].i += scratch[3].i;
        f_out[m2].r = f_out[m].r + scratch[0].i;
        f_out[m2].i = f_out[m].i - scratch[0].r;
        f_out[m].r -= scratch[0].i;
        f_out[m].i += scratch[0].r;
        f_out = &mut f_out[1_usize..];
        k = k.wrapping_sub(1);
        if k == 0 {
            break;
        }
    }
}
fn kf_bfly5(f_out: &mut [Complex], fstride: usize, st: &mut State, m: usize) {
    let mut scratch: [Complex; 13] = [Complex { r: 0, i: 0 }; 13];
    let twiddles = &mut st.twiddles;
    let ya = twiddles[fstride * m];
    let yb = twiddles[2 * fstride * m];
    let (mut f_out_0, f_out_1) = f_out.split_at_mut(m);
    let (mut f_out_1, f_out_2) = f_out_1.split_at_mut(m);
    let (mut f_out_2, f_out_3) = f_out_2.split_at_mut(m);
    let (mut f_out_3, mut f_out_4) = f_out_3.split_at_mut(m);
    let mut u = 0;
    while u < m {
        f_out_0[0].r =
            ((f_out_0[0].r as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_0[0].i =
            ((f_out_0[0].i as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_1[0].r =
            ((f_out_1[0].r as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_1[0].i =
            ((f_out_1[0].i as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_2[0].r =
            ((f_out_2[0].r as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_2[0].i =
            ((f_out_2[0].i as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_3[0].r =
            ((f_out_3[0].r as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_3[0].i =
            ((f_out_3[0].i as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_4[0].r =
            ((f_out_4[0].r as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_4[0].i =
            ((f_out_4[0].i as i64 * (2147483647 / 5) as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[0] = f_out_0[0];
        scratch[1].r = ((f_out_1[0].r as i64 * (twiddles[u * fstride]).r as i64
            - f_out_1[0].i as i64 * (twiddles[(u).wrapping_mul(fstride)]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[1].i = ((f_out_1[0].r as i64 * (twiddles[(u).wrapping_mul(fstride)]).i as i64
            + f_out_1[0].i as i64 * (twiddles[(u).wrapping_mul(fstride)]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[2].r = ((f_out_2[0].r as i64 * (twiddles[(2 * u).wrapping_mul(fstride)]).r as i64
            - f_out_2[0].i as i64 * (twiddles[(2 * u).wrapping_mul(fstride)]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[2].i = ((f_out_2[0].r as i64 * (twiddles[(2 * u).wrapping_mul(fstride)]).i as i64
            + f_out_2[0].i as i64 * (twiddles[(2 * u).wrapping_mul(fstride)]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[3].r = ((f_out_3[0].r as i64 * (twiddles[(3 * u).wrapping_mul(fstride)]).r as i64
            - f_out_3[0].i as i64 * (twiddles[(3 * u).wrapping_mul(fstride)]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[3].i = ((f_out_3[0].r as i64 * (twiddles[(3 * u).wrapping_mul(fstride)]).i as i64
            + f_out_3[0].i as i64 * (twiddles[(3 * u).wrapping_mul(fstride)]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[4].r = ((f_out_4[0].r as i64 * (twiddles[(4 * u).wrapping_mul(fstride)]).r as i64
            - f_out_4[0].i as i64 * (twiddles[(4 * u).wrapping_mul(fstride)]).i as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[4].i = ((f_out_4[0].r as i64 * (twiddles[(4 * u).wrapping_mul(fstride)]).i as i64
            + f_out_4[0].i as i64 * (twiddles[(4 * u).wrapping_mul(fstride)]).r as i64
            + (1 << (31 - 1)) as i64)
            >> 31) as i32;
        scratch[7].r = scratch[1].r + scratch[4].r;
        scratch[7].i = scratch[1].i + scratch[4].i;
        scratch[10].r = scratch[1].r - scratch[4].r;
        scratch[10].i = scratch[1].i - scratch[4].i;
        scratch[8].r = scratch[2].r + scratch[3].r;
        scratch[8].i = scratch[2].i + scratch[3].i;
        scratch[9].r = scratch[2].r - scratch[3].r;
        scratch[9].i = scratch[2].i - scratch[3].i;
        f_out_0[0].r += scratch[7].r + scratch[8].r;
        f_out_0[0].i += scratch[7].i + scratch[8].i;
        scratch[5].r = scratch[0].r
            + ((scratch[7].r as i64 * ya.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32
            + ((scratch[8].r as i64 * yb.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[5].i = scratch[0].i
            + ((scratch[7].i as i64 * ya.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32
            + ((scratch[8].i as i64 * yb.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[6].r = ((scratch[10].i as i64 * ya.i as i64 + (1 << (31 - 1)) as i64) >> 31) as i32
            + ((scratch[9].i as i64 * yb.i as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[6].i = -(((scratch[10].r as i64 * ya.i as i64 + (1 << (31 - 1)) as i64) >> 31)
            as i32)
            - ((scratch[9].r as i64 * yb.i as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_1[0].r = scratch[5].r - scratch[6].r;
        f_out_1[0].i = scratch[5].i - scratch[6].i;
        f_out_4[0].r = scratch[5].r + scratch[6].r;
        f_out_4[0].i = scratch[5].i + scratch[6].i;
        scratch[11].r = scratch[0].r
            + ((scratch[7].r as i64 * yb.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32
            + ((scratch[8].r as i64 * ya.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[11].i = scratch[0].i
            + ((scratch[7].i as i64 * yb.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32
            + ((scratch[8].i as i64 * ya.r as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[12].r = -(((scratch[10].i as i64 * yb.i as i64 + (1 << (31 - 1)) as i64) >> 31)
            as i32)
            + ((scratch[9].i as i64 * ya.i as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        scratch[12].i = ((scratch[10].r as i64 * yb.i as i64 + (1 << (31 - 1)) as i64) >> 31)
            as i32
            - ((scratch[9].r as i64 * ya.i as i64 + (1 << (31 - 1)) as i64) >> 31) as i32;
        f_out_2[0].r = scratch[11].r + scratch[12].r;
        f_out_2[0].i = scratch[11].i + scratch[12].i;
        f_out_3[0].r = scratch[11].r - scratch[12].r;
        f_out_3[0].i = scratch[11].i - scratch[12].i;
        f_out_0 = &mut f_out_0[1..];
        f_out_1 = &mut f_out_1[1..];
        f_out_2 = &mut f_out_2[1..];
        f_out_3 = &mut f_out_3[1..];
        f_out_4 = &mut f_out_4[1..];
        u += 1;
    }
}

fn kf_work_120(f_out: &mut [Complex], f: &[Complex], st: &mut State) {
    let mut counter_0: core::ffi::c_int = 0 as core::ffi::c_int;
    let mut counter_1: core::ffi::c_int = 0 as core::ffi::c_int;
    let mut counter_2: core::ffi::c_int = 0 as core::ffi::c_int;
    for i in 0..24 {
        let f_tmp = &f[FREQUENCY_OFFSET_120[i] as usize..];
        f_out[i * 5] = f_tmp[0];
        f_out[i * 5 + 1] = f_tmp[24];
        f_out[i * 5 + 2] = f_tmp[48];
        f_out[i * 5 + 3] = f_tmp[72];
        f_out[i * 5 + 4] = f_tmp[96];
        kf_bfly5(&mut f_out[i * 5..], 24, st, 1);
        counter_0 += 1;
        if counter_0 == 3 as core::ffi::c_int {
            kf_bfly3(&mut f_out[(i - 2) * 5..], 8, st, 5);
            counter_1 += 1;
            counter_0 = 0 as core::ffi::c_int;
        }
        if counter_1 == 2 as core::ffi::c_int {
            kf_bfly2(&mut f_out[(i - 5) * 5..], 4 as core::ffi::c_int as usize, st, 15 as core::ffi::c_int);
            counter_2 += 1;
            counter_1 = 0 as core::ffi::c_int;
        }
        if counter_2 == 4 as core::ffi::c_int {
            kf_bfly4(&mut f_out[(i - 23) * 5..], 1, st, 30);
        }
    }
}

fn kf_work_240(f_out: &mut [Complex], f: &[Complex], st: &mut State) {
    let mut counter_0: core::ffi::c_int = 0 as core::ffi::c_int;
    let mut counter_1: core::ffi::c_int = 0 as core::ffi::c_int;
    let mut counter_2: core::ffi::c_int = 0 as core::ffi::c_int;
    for i in 0..48 {
        let f_tmp = &f[FREQUENCY_OFFSET_240[i] as usize..];
        f_out[i * 5] = f_tmp[0];
        f_out[i * 5 + 1] = f_tmp[48];
        f_out[i * 5 + 2] = f_tmp[96];
        f_out[i * 5 + 3] = f_tmp[144];
        f_out[i * 5 + 4] = f_tmp[192];
        kf_bfly5(&mut f_out[i * 5..], 48, st, 1);
        counter_0 += 1;
        if counter_0 == 3 as core::ffi::c_int {
            kf_bfly3(&mut f_out[(i + 1) * 5 - 15..], 16, st, 5);
            counter_1 += 1;
            counter_0 = 0 as core::ffi::c_int;
        }
        if counter_1 == 4 as core::ffi::c_int {
            kf_bfly4(&mut f_out[(i + 1) * 5 - 60..], 4, st, 15);
            counter_2 += 1;
            counter_1 = 0 as core::ffi::c_int;
        }
        if counter_2 == 4 as core::ffi::c_int {
            kf_bfly4(&mut f_out[(i + 1) * 5 - 240..], 1, st, 60);
        }
    }
}

fn kf_work_480(f_out: &mut [Complex], f: &[Complex], st: &mut State) {
    let mut counter_0: core::ffi::c_int = 0 as core::ffi::c_int;
    let mut counter_1: core::ffi::c_int = 0 as core::ffi::c_int;
    let mut counter_2: core::ffi::c_int = 0 as core::ffi::c_int;
    let mut counter_3: core::ffi::c_int = 0 as core::ffi::c_int;
    for i in 0..96 {
        let f_tmp = &f[FREQUENCY_OFFSET_480[i] as usize..];
        f_out[i * 5] = f_tmp[0];
        f_out[i * 5 + 1] = f_tmp[96];
        f_out[i * 5 + 2] = f_tmp[192];
        f_out[i * 5 + 3] = f_tmp[288];
        f_out[i * 5 + 4] = f_tmp[384];
        kf_bfly5(&mut f_out[(i + 1) * 5 - 5..], 96, st, 1);
        counter_0 += 1;
        if counter_0 == 3 as core::ffi::c_int {
            kf_bfly3(&mut f_out[(i + 1) * 5 - 15..], 32, st, 5);
            counter_1 += 1;
            counter_0 = 0 as core::ffi::c_int;
        }
        if counter_1 == 2 as core::ffi::c_int {
            kf_bfly2(&mut f_out[(i + 1) * 5 - 30..], 16, st, 15);
            counter_2 += 1;
            counter_1 = 0 as core::ffi::c_int;
        }
        if counter_2 == 4 as core::ffi::c_int {
            kf_bfly4(&mut f_out[(i + 1) * 5 - 120..], 4, st, 30);
            counter_3 += 1;
            counter_2 = 0 as core::ffi::c_int;
        }
        if counter_3 == 4 as core::ffi::c_int {
            kf_bfly4(&mut f_out[(i + 1) * 5 - 480..], 1, st, 120);
        }
    }
}

static FREQUENCY_OFFSET_120: [u8; 24] =
    [0, 8, 16, 4, 12, 20, 1, 9, 17, 5, 13, 21, 2, 10, 18, 6, 14, 22, 3, 11, 19, 7, 15, 23];

static FREQUENCY_OFFSET_240: [u8; 48] = [
    0, 16, 32, 4, 20, 36, 8, 24, 40, 12, 28, 44, 1, 17, 33, 5, 21, 37, 9, 25, 41, 13, 29, 45, 2,
    18, 34, 6, 22, 38, 10, 26, 42, 14, 30, 46, 3, 19, 35, 7, 23, 39, 11, 27, 43, 15, 31, 47,
];

static FREQUENCY_OFFSET_480: [u8; 96] = [
    0, 32, 64, 16, 48, 80, 4, 36, 68, 20, 52, 84, 8, 40, 72, 24, 56, 88, 12, 44, 76, 28, 60, 92, 1,
    33, 65, 17, 49, 81, 5, 37, 69, 21, 53, 85, 9, 41, 73, 25, 57, 89, 13, 45, 77, 29, 61, 93, 2,
    34, 66, 18, 50, 82, 6, 38, 70, 22, 54, 86, 10, 42, 74, 26, 58, 90, 14, 46, 78, 30, 62, 94, 3,
    35, 67, 19, 51, 83, 7, 39, 71, 23, 55, 87, 11, 43, 75, 27, 59, 91, 15, 47, 79, 31, 63, 95,
];

fn kf_factor(mut n: i32, facbuf: &mut [i32]) {
    let mut p: i32 = 4;
    let floor_sqrt = (math::sqrt(n as f32) + 0.5f32) as i32;
    let mut i = 0;
    loop {
        while n % p != 0 {
            match p {
                4 => {
                    p = 2;
                }
                2 => {
                    p = 3;
                }
                _ => {
                    p += 2;
                }
            }
            if p > floor_sqrt {
                p = n;
            }
        }
        n /= p;
        facbuf[i] = p;
        i += 1;
        facbuf[i] = n;
        i += 1;
        if n <= 1 {
            break;
        }
    }
}

pub fn lhdc_fft(cfg: &mut State, f_in: &[Complex], f_out: &mut [Complex]) {
    match cfg.nfft {
        120 => {
            kf_work_120(f_out, f_in, cfg);
        }
        240 => {
            kf_work_240(f_out, f_in, cfg);
        }
        480 => {
            kf_work_480(f_out, f_in, cfg);
        }
        _ => {}
    };
}
