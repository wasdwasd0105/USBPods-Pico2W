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

#[derive(Copy, Clone)]
pub struct RingBuf {
    buf: [u8; 128],
    idx: i32,
    max_shifts: i32,
    shifts_done: i32,
}

impl Default for RingBuf {
    fn default() -> Self {
        Self { buf: [0; 128], idx: 0, max_shifts: 0, shifts_done: 0 }
    }
}

impl RingBuf {
    pub fn peek(&mut self) -> u8 {
        self.buf[self.idx as usize]
    }
    pub fn push(&mut self, val: u8) {
        self.buf[self.idx as usize] = val;
        self.idx += 1;
        if self.idx >= self.max_shifts {
            self.idx = 0;
        }
    }
    /// Returns true for shift successful, false for no more room
    pub fn shift(&mut self, val: u8) -> bool {
        if self.shifts_done >= self.max_shifts {
            false
        } else {
            self.buf[self.shifts_done as usize] = val;
            self.shifts_done += 1;
            true
        }
    }
    pub fn reset(&mut self, max_shifts: i32) {
        self.shifts_done = 0;
        self.max_shifts = max_shifts;
        self.idx = 0;
    }
}
