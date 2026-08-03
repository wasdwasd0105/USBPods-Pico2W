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

use crate::math;

fn gmax_calc(a: f32, b: f32, c: f32, d: i32, len: i32, e: i32) -> f32 {
    a - (b - c) / d as f32 * (len - e) as f32
}

const OFFSET_MAX: i32 = (1 << 9) - 8;

pub struct LevelTable {
    cache: [f32; 512],
    // TODO(b/454096420) make non-pub
    pub start: i32,
    jump: f32,
}

impl LevelTable {
    /// Constructs a zeroed `LevelTable`
    ///
    /// Results are not meaningful until `init` is called.
    pub fn new() -> Self {
        Self { cache: [0.0; 512], start: 0, jump: 0.0 }
    }

    pub fn level(&mut self, offset_idx: i32) -> f32 {
        if self.cache[offset_idx as usize] != 0.0 {
            return self.cache[offset_idx as usize];
        }
        if offset_idx > self.start {
            self.cache[offset_idx as usize] = 1.0f32 / math::power_of_2(self.calc(offset_idx));
        } else {
            self.cache[offset_idx as usize] = 1.0f32 / (1_i32 << offset_idx) as f32;
        };
        self.cache[offset_idx as usize]
    }

    /// Computes a function of `offset_idx` with:
    /// * Slope 1 from 0 to `start`
    /// * Slope offset_jump from `start` to `OFFSET_MAX`
    /// * Slope 1 thereafter
    /// * A maximum of 30.0
    fn calc(&self, offset_idx: i32) -> f32 {
        let mut offset =
            (offset_idx.min(OFFSET_MAX) - self.start) as f32 * self.jump + self.start as f32;
        offset += (offset_idx.max(OFFSET_MAX) - OFFSET_MAX) as f32;
        offset.min(30.0)
    }

    pub fn init(&mut self, size: i32, resolution: i32, hz: i32, ms: i32) {
        self.start = 1;
        self.cache.fill(0.0);

        let mut offset_max = 30.0f32;

        if ms % 25 == 0 && ms <= 100 {
            match resolution {
                24 => {
                    match hz {
                        8000 | 16000 | 24000 | 32000 | 44100 | 48000 => {
                            offset_max = gmax_calc(26.2, 26.2, 16.4, 575, size, 50);
                        }
                        96000 => {
                            offset_max = gmax_calc(25.7, 25.7, 20.75, 575, size, 50);
                        }
                        192000 => {
                            offset_max = gmax_calc(25.7, 25.7, 24.42, 575, size, 50);
                        }
                        _ => {}
                    }
                    // TODO(b/454096420): explicit ceil / floor selection
                    self.start = (offset_max / 4.0) as i32;
                }
                16 => {
                    match hz {
                        8000 | 16000 | 24000 | 32000 | 44100 => {
                            offset_max = gmax_calc(26.15, 26.15, 16.42, 575, size, 50);
                        }
                        48000 => {
                            offset_max = gmax_calc(26.18, 26.18, 16.32, 575, size, 50);
                        }
                        _ => {}
                    }
                    // TODO(b/454096420): explicit ceil / floor selection
                    self.start = (offset_max / 2.5) as i32;
                }
                _ => {}
            }
        }

        let deno = OFFSET_MAX - self.start - 1;
        self.jump = (offset_max - self.start as f32) / deno as f32;
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn init() {
        let test_vectors = [
            (16, 48000, 25, 10, 0.031080343),
            (16, 44100, 25, 10, 0.03104242),
            (24, 48000, 25, 6, 0.038929228),
            (24, 96000, 25, 6, 0.038771763),
            (24, 192000, 25, 6, 0.039413873),
            (16, 48000, 30, 1, 0.057768926),
            (16, 44100, 30, 1, 0.057768926),
            (24, 48000, 30, 1, 0.057768926),
            (24, 96000, 30, 1, 0.057768926),
            (24, 192000, 30, 1, 0.057768926),
        ];

        let mut level_table = LevelTable::new();
        for (resolution, hz, ms, start, jump) in test_vectors {
            level_table.init(100 /*size*/, resolution, hz, ms);
            assert_eq!(level_table.start, start);
            assert_eq!(level_table.jump, jump);
        }
    }
}
