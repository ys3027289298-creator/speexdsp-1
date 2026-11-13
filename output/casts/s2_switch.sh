set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 2: in-call switch 48k -> 44.1k -> 32k, one state, no restart"
./libtool --mode=execute libspeexdsp/testresample3
echo
title "Switch timing / continuity stats"
cat output/testresample3_stats.txt
