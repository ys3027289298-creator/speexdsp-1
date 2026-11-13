#!/usr/bin/env bash
# Segmented, fully non-interactive demo recording.
# Each segment prints a title, pauses before/after its commands and never
# waits for keyboard input. Produces one asciinema cast per segment plus
# a combined cast in output/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/output"
CAST_DIR="$OUT/casts"
mkdir -p "$CAST_DIR"

title() {
  local text="$1"
  printf '\n\033[1;36m============================================================\n'
  printf ' %s\n' "$text"
  printf '============================================================\033[0m\n'
}

pause() {
  # Visible inter-segment pause, no keyboard input required.
  local seconds="$1"
  for i in $(seq "$seconds" -1 1); do
    printf '\r\033[1;33m--- next segment in %ds ---\033[0m' "$i"
    sleep 1
  done
  printf '\r%*s\r' 40 ""
}

# Helpers reused by the per-segment command scripts as well.
cat > "$CAST_DIR/demo_lib.sh" <<'EOLIB'
title() {
  local text="$1"
  printf '\n\033[1;36m============================================================\n'
  printf ' %s\n' "$text"
  printf '============================================================\033[0m\n'
}
pause() {
  local seconds="$1"
  for i in $(seq "$seconds" -1 1); do
    printf '\r\033[1;33m--- next segment in %ds ---\033[0m' "$i"
    sleep 1
  done
  printf '\r%*s\r' 40 ""
}
EOLIB

run_segment() {
  local index="$1" heading="$2" script_file="$3" cast_file="$4"
  local runner="$CAST_DIR/run_segment_${index}.sh"
 cat > "$runner" <<EOR
set -e
ROOT="$ROOT"
OUT="$OUT"
source "\$(dirname "\$0")/demo_lib.sh"
title '$heading [$index/4]'
pause 2
bash '$script_file'
pause 2
EOR
  asciinema rec --overwrite --output-format asciicast-v2 \
    --command "bash '$runner'" "$cast_file"
}

# Per-segment command scripts.
cat > "$CAST_DIR/s1_build.sh" <<'EOS'
set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 1: build libspeexdsp with the new test entry"
make -j8 2>&1 | tail -4
ls -l libspeexdsp/testresample3
EOS

cat > "$CAST_DIR/s2_switch.sh" <<'EOS'
set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 2: in-call switch 48k -> 44.1k -> 32k, one state, no restart"
./libtool --mode=execute libspeexdsp/testresample3
echo
title "Switch timing / continuity stats"
cat output/testresample3_stats.txt
EOS

cat > "$CAST_DIR/s3_verify.sh" <<'EOS'
set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 3: offline equivalence, continuity, latency, error codes"
python3 output/verify_switch.py
EOS

cat > "$CAST_DIR/s4_artifacts.sh" <<'EOS'
set -e
source "/home/ys/feedback/uidreczz28YpbZJShIo/output/casts/demo_lib.sh"
cd "$ROOT"
title "Segment 4: artifacts in output/ (audio + reports)"
ls -l output
echo
echo "Per-rate rendered segments:"
ls -l output/segment*.wav
EOS

run_segment 1 "Build"            "$CAST_DIR/s1_build.sh"  "$CAST_DIR/segment1_build.cast"
run_segment 2 "Live switching"   "$CAST_DIR/s2_switch.sh" "$CAST_DIR/segment2_switch.cast"
run_segment 3 "Verification"     "$CAST_DIR/s3_verify.sh" "$CAST_DIR/segment3_verify.cast"
run_segment 4 "Artifacts"        "$CAST_DIR/s4_artifacts.sh" "$CAST_DIR/segment4_artifacts.cast"

# Combine the four casts into one replayable file (v2 format).
python3 - "$CAST_DIR" "$OUT/demo_combined.cast" <<'EOCOMBINE'
import json, sys
cast_dir, out_path = sys.argv[1], sys.argv[2]
names = ["segment1_build.cast", "segment2_switch.cast",
         "segment3_verify.cast", "segment4_artifacts.cast"]
events = []
header = None
offset = 0.0
for name in names:
    with open("%s/%s" % (cast_dir, name)) as f:
        header = json.loads(f.readline())
        for line in f:
            record = json.loads(line)
            record[0] = record[0] + offset + 1.0
            events.append(record)
    offset = events[-1][0]
with open(out_path, "w") as f:
    f.write(json.dumps(header) + "\n")
    for ev in events:
        f.write(json.dumps(ev) + "\n")
print("combined cast:", out_path, "events:", len(events))
EOCOMBINE

echo "All casts written to $CAST_DIR and $OUT/demo_combined.cast"
