#!/bin/bash
# VK-GL-CTS 1.4.6.2 (deqp-vk) + deqp-runner for the render-pass overlap tests;
# scratch tree in /var/tmp/cts (run as alarm; tests run as arch, cts-run.sh)
set -ex
W=/var/tmp/cts
mkdir -p $W && cd $W
[ -d VK-GL-CTS ] || git clone --depth 1 -b vulkan-cts-1.4.6.2 https://github.com/KhronosGroup/VK-GL-CTS.git
cd VK-GL-CTS
python3 external/fetch_sources.py
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=default -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
nice ninja -C build -j6 deqp-vk
CARGO_HOME=$W/cargo cargo install --locked deqp-runner
echo CTS-BUILD-DONE
