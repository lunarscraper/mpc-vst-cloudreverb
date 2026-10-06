#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan) of cloudreverb_vst.cpp, then a CPU benchmark (-O3, as on the device): run after build.sh
# (needs build/params.h, build/popup.h). Prints PASSED/FAILED; exit code follows.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/b -w /b gcc:12 bash -euc '
  mkdir -p build/x86
  SAN="-fsanitize=address,undefined"
  g++ -O1 -g $SAN -std=c++17 -fPIC -shared -Ibuild -I. -o build/x86/cloudreverb-x86.so cloudreverb_vst.cpp -lpthread
  g++ -O0 -g $SAN -std=c++17 -o build/x86/host_test host_test.cpp -ldl
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/cloudreverb-x86.so
  g++ -O3 -fno-math-errno -std=c++17 -fPIC -shared -fvisibility=hidden -Ibuild -I. -o build/x86/cloudreverb-o3.so cloudreverb_vst.cpp -lpthread
  g++ -O2 -std=c++17 -o build/x86/bench host_test.cpp -ldl
  ./build/x86/bench ./build/x86/cloudreverb-o3.so bench
'
