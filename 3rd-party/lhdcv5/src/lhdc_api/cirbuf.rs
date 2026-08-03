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
pub struct CircularBuffer {
    idx: usize,
    odx: usize,
    s_len: usize,
    r_len: usize,
    max_len: usize,
    // This is a weird accounting piece - while the get() API doesn't assume you fetch the whole
    // buffer, this is zeroed after the only get() done in the codebase. put() increments it, and
    // now returns it. It's essentially to record how many frames were stashed in the buffer to
    // tell when to call get()
    //
    // Ideally, it shouldn't be in this data structure, but moving it would be later cleanup
    item_cnt: usize,
    cbuf: Box<[u8]>,
}

impl CircularBuffer {
    pub fn new(len: usize) -> Self {
        Self {
            idx: 0,
            odx: 0,
            s_len: 0,
            max_len: len,
            r_len: len,
            cbuf: core::iter::repeat_n(0, len).collect(),
            item_cnt: 0,
        }
    }
    pub fn reset(&mut self) {
        self.idx = 0;
        self.odx = 0;
        self.s_len = 0;
        self.r_len = self.max_len;
    }
    pub fn len(&self) -> usize {
        self.s_len
    }
    pub fn is_empty(&self) -> bool {
        self.s_len == 0
    }
    pub fn empty_len(&self) -> usize {
        self.r_len
    }
    pub fn get(&mut self, mut buf: &mut [u8]) {
        self.item_cnt = 0;
        if buf.is_empty() || self.is_empty() {
            return;
        }
        if buf.len() > self.len() {
            buf = &mut buf[0..self.len()];
        }
        let mut t_len = buf.len();
        let read_len = t_len;
        let n = self.max_len - self.odx;
        if n <= t_len {
            buf[0..n].copy_from_slice(&self.cbuf[self.odx..]);
            buf = &mut buf[n..];
            t_len -= n;
            self.odx = 0;
        }
        if t_len != 0 {
            buf[0..t_len].copy_from_slice(&self.cbuf[self.odx..self.odx + t_len]);
            self.odx += t_len;
        }
        self.s_len -= read_len;
        self.r_len += read_len;
    }
    pub fn put(&mut self, mut buf: &[u8]) -> usize {
        self.item_cnt += 1;
        if self.empty_len() == 0 || buf.is_empty() {
            return 0;
        }
        if buf.len() > self.r_len {
            buf = &buf[0..self.r_len];
        }
        let mut t_len = buf.len();
        let write_len = t_len;
        let n = self.max_len - self.idx;
        if n <= t_len {
            self.cbuf[self.idx..].copy_from_slice(&buf[..n]);
            buf = &buf[n..];
            t_len -= n;
            self.idx = 0;
        }
        if t_len != 0 {
            self.cbuf[self.idx..self.idx + t_len].copy_from_slice(&buf[..t_len]);
            self.idx += t_len;
        }
        self.s_len += write_len;
        self.r_len -= write_len;
        self.item_cnt
    }
}
