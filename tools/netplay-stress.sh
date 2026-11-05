#!/bin/bash
# Rollback stress run: one determinism-harness race per track and arena, rolling back 8 frames after every frame and
# checking each re-simulated frame against the first run (SK_DETERMINISM_ROLLBACK, see DeterminismHarness.h).
#
#   tools/netplay-stress.sh [game executable]
#
# The executable defaults to build-cmake's. Give it an AddressSanitizer build to look for memory errors as well
# (docs/NETPLAY.md, Testing). Each race gets a throwaway settings folder. Options, as environment variables:
#   FRAMES=2000   race frames (30 per second)
#   JOBS=4        races at a time
#   O2R=...       the extracted mk64.o2r (default: the game's folder)
# Prints one line per race: its exit code (0 = in sync), seconds, and AddressSanitizer reports.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
EXE=${1:-$REPO/build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart}
FRAMES=${FRAMES:-2000}
JOBS=${JOBS:-4}
O2R=${O2R:-$HOME/Library/Application Support/SpaghettiKart/mk64.o2r}
OUT=${TMPDIR:-/tmp}/netplay-stress/run-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "Logs: $OUT"

# run <mode> <track> <players>
run() {
    local mode=$1 track=$2 players=$3
    local name=$mode-$track
    local home=$OUT/home-$name
    mkdir -p "$home"
    cat > "$home/spaghettify.cfg.json" <<EOF
{ "CVars": { "gSkipIntro": 3, "gVsyncEnabled": 0, "gInterpolationFPS": 30 },
  "Window": { "Width": 400, "Height": 300, "PositionX": 40, "PositionY": 60 } }
EOF
    ln -sf "$O2R" "$home/mk64.o2r"
    local start=$(date +%s)
    # The arena copies vector buffers whole, which the container-overflow check would report. After a crash, macOS
    # asks whether to reopen the game's windows and the game waits; -ApplePersistenceIgnoreState skips that.
    SHIP_HOME=$home SK_FAST=10 ASAN_OPTIONS=detect_container_overflow=0:halt_on_error=0 \
        SK_DETERMINISM_RECORD=$OUT/$name.skdt SK_DETERMINISM_FRAMES=$FRAMES SK_DETERMINISM_MODE=$mode \
        SK_DETERMINISM_TRACK=mk:$track SK_DETERMINISM_PLAYERS=$players SK_DETERMINISM_ROLLBACK=8 \
        timeout 900 "$EXE" -ApplePersistenceIgnoreState YES > "$OUT/$name.out" 2>&1
    local code=$?
    local reports=$(grep -a -c "ERROR: AddressSanitizer" "$OUT/$name.out")
    local failure=$(grep -a -m1 "Determinism\] FAIL" "$OUT/$name.out")
    printf "%-26s exit %-3s %4ss  %s AddressSanitizer reports  %s\n" "$name" "$code" "$(( $(date +%s) - start ))" \
        "$reports" "$failure"
}
export -f run
export OUT EXE FRAMES O2R

{
    for track in luigi_raceway moo_moo_farm koopa_troopa_beach kalimari_desert toads_turnpike frappe_snowland \
        choco_mountain mario_raceway wario_stadium sherbet_land royal_raceway bowsers_castle dk_jungle yoshi_valley \
        banshee_boardwalk rainbow_road; do
        echo "vs $track 2"
    done
    for arena in big_donut block_fort double_deck skyscraper; do
        echo "battle $arena 4"
    done
    for track in luigi_raceway frappe_snowland royal_raceway rainbow_road; do
        echo "gp $track 2"
    done
} | xargs -P "$JOBS" -L 1 bash -c 'run "$@"' _

echo "AddressSanitizer reports, by place:"
cat "$OUT"/*.out | grep -a "^SUMMARY: AddressSanitizer" | sed -E 's/ \(SpaghettiKart:[^)]*\)//; s/\+0x[0-9a-f]+$//' |
    sort | uniq -c | sort -rn
