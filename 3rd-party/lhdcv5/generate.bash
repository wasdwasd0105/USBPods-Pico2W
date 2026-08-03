#!/bin/bash
set -e
parallel cargo run --release --bin simulator -- {1} {2} :::: <(ls samples/*.wav savi_test_sample/*.wav) ::: 64000 128000 192000 256000 320000 400000 500000 900000 1000000 0
