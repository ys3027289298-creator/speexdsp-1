set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 4: artifacts in output/ (audio + reports)"
ls -l output
echo
echo "Per-rate rendered segments:"
ls -l output/segment*.wav
