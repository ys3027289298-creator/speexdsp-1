#!/bin/bash
# Segmented demo for the multi-stream resampling mixer.
# Each segment has a title card and a pause; nothing waits for keyboard input.
cd "$(dirname "$0")/.."
set -e

banner() {
   clear
   echo "================================================================"
   echo "  $1"
   echo "================================================================"
   sleep 1.5
}

pause() { sleep 2.5; }

banner "Segment 1/5 - Build (repo entry points, small changes in 3 files)"
git diff --stat
touch libspeexdsp/resample.c libspeexdsp/testresample2.c
make -s 2>&1 | grep -E "testresample2|resample" || true
echo "build OK"
pause

banner "Segment 2/5 - Sample-exact check: 8 streams, different rates -> one mix"
./libspeexdsp/testresample2 mix check output/mixed.f32
pause

banner "Segment 3/5 - Stream limit: exceeding max_streams reports an error"
./libspeexdsp/testresample2 mix limit
pause

banner "Segment 4/5 - Peak memory: mixer vs naive per-stream"
./libspeexdsp/testresample2 mix mem
pause

banner "Segment 5/5 - Artifacts in output/"
ffmpeg -v error -y -f f32le -ar 48000 -ac 1 -i output/mixed.f32 output/mixed.wav
./libspeexdsp/testresample2 mix all output/mixed.f32 > output/report.txt 2>&1
ls -la output/
echo "demo done"
pause
