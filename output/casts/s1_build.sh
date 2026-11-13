set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 1: build libspeexdsp with the new test entry"
make -j8 2>&1 | tail -4
ls -l libspeexdsp/testresample3
