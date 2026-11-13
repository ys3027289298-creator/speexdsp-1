set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 3: offline equivalence, continuity, latency, error codes"
python3 output/verify_switch.py
