#!/bin/sh
# Tests raster-tspl against the vendor's x86 filter (run under Rosetta).
#
# 1. Identity: both filters with the vendor PPDs must produce the same bytes
#    and exit status, for every model, option and out-of-range value.
# 2. Fixed vendor bugs: our output for the buggy input must equal the vendor's
#    output for the corrected input (Yes -> True, bad Orientation -> 0,
#    NumCopies 0 -> 1).
# 3. Our PPDs: SIZE and bitmap width follow the true inch dimensions.
#
# Usage: test/compare.sh [vendor-filter] [vendor-ppd-dir]   (run from the repo root on a Mac, after make)
set -e
VENDOR=${1:-/usr/libexec/cups/filter/raster-tspl}
# Use the vendor backup when the native filter has been installed in its place.
[ -z "$1" ] && [ -x "$VENDOR.x86_64-vendor" ] && VENDOR=$VENDOR.x86_64-vendor
VPPDS=${2:-/Library/Printers/PPDs/Contents/Resources}
NATIVE=$(pwd)/raster-tspl
OURS=$(pwd)/build
TEST=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

[ -x "$NATIVE" ] && [ -f "$OURS/cx418.ppd" ] || { echo "run make first"; exit 1; }
MODELS="CX418 CX418BT CX418HD CE410 CI250 CI250E"

for m in $MODELS; do
    gunzip -c "$VPPDS/$m.TSPL.ppd.gz" > "$T/vendor-$m.ppd"
done

python3 -I "$TEST/make_label.py" "$T/label.pdf"
for dpi in 203 300; do
    p="$T/vendor-CX418.ppd"
    [ $dpi = 300 ] && p="$T/vendor-CX418HD.ppd"
    cupsfilter -p "$p" -m application/vnd.cups-raster "$T/label.pdf" > "$T/gray8-$dpi.ras" 2>/dev/null
    cupsfilter -p "$p" -o ColorOption=GrayScale -m application/vnd.cups-raster "$T/label.pdf" > "$T/gray1-$dpi.ras" 2>/dev/null
    cupsfilter -p "$p" -o PageSize=w50h25 -m application/vnd.cups-raster "$T/label.pdf" > "$T/small-$dpi.ras" 2>/dev/null
    cupsfilter -p "$p" -o PageSize=w100h330 -m application/vnd.cups-raster "$T/label.pdf" > "$T/long-$dpi.ras" 2>/dev/null
    # A CUPS raster stream is one sync word followed by pages. NumCopies sits
    # at byte 340 of each page header.
    python3 -I - "$T" "$dpi" <<'PY'
import struct, sys
t, dpi = sys.argv[1], sys.argv[2]
d = open(f"{t}/gray1-{dpi}.ras", "rb").read()
def page(n):
    e = bytearray(d); struct.pack_into("<I", e, 4 + 340, n); return bytes(e[4:])
def job(name, *copies):
    open(f"{t}/{name}-{dpi}.ras", "wb").write(d[:4] + b"".join(page(n) for n in copies))
g8 = open(f"{t}/gray8-{dpi}.ras", "rb").read()
open(f"{t}/twopage-{dpi}.ras", "wb").write(g8 + g8[4:])
job("copies1", 1); job("copies3", 3); job("copies11", 1, 1); job("copies211", 2, 1, 1)
job("copies0", 0); job("copies00", 0, 0); job("copies01", 0, 1); job("copies201", 2, 0, 1)
PY
done

pass=0; fail=0
# check model raster options [vendor-raster vendor-options]
# Runs ours with (raster, options) and the vendor with the overrides, if given.
check() {
    a=$( (PPD="$T/vendor-$1.ppd" "$VENDOR" 1 u t 1 "${5-$3}" < "$T/${4:-$2}" 2>/dev/null; echo "exit=$?") | shasum)
    b=$( (PPD="$T/vendor-$1.ppd" "$NATIVE" 1 u t 1 "$3" < "$T/$2" 2>/dev/null; echo "exit=$?") | shasum)
    if [ "$a" = "$b" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $1 $2 [$3]"; fi
}

cases() {  # dpi
    d=$1
    cat <<CASES
gray8-$d.ras|
gray1-$d.ras|
twopage-$d.ras|PrintDarkness=8
small-$d.ras|PageSize=w50h25
small-$d.ras|PageSize=w50h25 PaperType=2 PrintSpeed=2
long-$d.ras|PageSize=w100h330
gray8-$d.ras|PrintDarkness=10 PrintSpeed=4 PaperType=1 ShiftMove=2 FowardOffset=-3 MediaMethod=1
$(for s in -12 -11 -3 -1 0 1 5 11 12; do echo "gray8-$d.ras|ShiftMove=$s"; done)
gray8-$d.ras|FowardOffset=30 PrintDarkness=0
gray8-$d.ras|FowardOffset=-30 PrintDarkness=15 PrintSpeed=3
gray8-$d.ras|PaperType=3 PostAction=None
gray8-$d.ras|PaperType=2
gray8-$d.ras|MirrorImage=True
gray8-$d.ras|NegativeImage=True
gray8-$d.ras|Orientation=2
gray8-$d.ras|Orientation=2 MirrorImage=True NegativeImage=True
gray1-$d.ras|Orientation=2 NegativeImage=True
twopage-$d.ras|Orientation=2 PaperType=1
gray8-$d.ras|DataCompress=True
gray8-$d.ras|DataCompress=False
long-$d.ras|PageSize=w100h330 DataCompress=True
twopage-$d.ras|DataCompress=True Orientation=2 NegativeImage=True
$(for o in 1 3; do for x in "" " MirrorImage=True" " DataCompress=True" " MirrorImage=True NegativeImage=True DataCompress=True"; do echo "gray8-$d.ras|Orientation=$o$x"; done; done)
copies3-$d.ras|
copies3-$d.ras|DataCompress=True PrintDarkness=5
CASES
}

# Every option against values outside the PPD's choices. The vendor passes most through.
# Values whose vendor behaviour we fixed map to the corrected vendor input.
fuzz() {  # dpi
    for opt in PrintSpeed PrintDarkness ShiftMove FowardOffset PaperType MediaMethod PostAction \
               Orientation MirrorImage NegativeImage DataCompress; do
        for v in "" None -1 0 1 2 3 4 5 7 9 10 16 99 abc 2.7 True False Yes No yes on off true TRUE; do
            vopt=""
            case "$opt" in
            MirrorImage|NegativeImage|DataCompress)
                case "$v" in Yes|yes|on|1) vopt="$opt=True";; esac;;
            Orientation)
                case "$v" in -1|4|5|7|9|10|16|99) vopt="$opt=0";; esac;;
            esac
            echo "gray1-$1.ras|$opt=$v||$vopt"
        done
    done
}

# Fixed vendor bugs, as ours|options|vendor-raster|vendor-options.
fixed() {  # dpi
    d=$1
    cat <<CASES
copies0-$d.ras||copies1-$d.ras|
copies00-$d.ras|PrintSpeed=3 PaperType=1|copies11-$d.ras|PrintSpeed=3 PaperType=1
copies01-$d.ras|PrintSpeed=3|copies11-$d.ras|PrintSpeed=3
copies201-$d.ras|PrintSpeed=3 DataCompress=True|copies211-$d.ras|PrintSpeed=3 DataCompress=True
copies0-$d.ras|Orientation=5 PaperType=1|copies1-$d.ras|Orientation=0 PaperType=1
gray8-$d.ras|Orientation=7 MirrorImage=Yes DataCompress=on|gray8-$d.ras|Orientation=0 MirrorImage=True DataCompress=True
CASES
}

for m in CX418 CX418BT CE410 CI250 CI250E; do
    cases 203 | while IFS='|' read -r ras opts; do echo "$m|$ras|$opts"; done
    fixed 203 | while IFS='|' read -r ras opts vras vopts; do echo "$m|$ras|$opts|$vras|$vopts|fixed"; done
done > "$T/cases"
cases 300 | while IFS='|' read -r ras opts; do echo "CX418HD|$ras|$opts"; done >> "$T/cases"
fixed 300 | while IFS='|' read -r ras opts vras vopts; do echo "CX418HD|$ras|$opts|$vras|$vopts|fixed"; done >> "$T/cases"
for m in CX418 CI250; do fuzz 203 | while IFS='|' read -r ras opts vras vopts; do echo "$m|$ras|$opts|$vras|$vopts"; done; done >> "$T/cases"
fuzz 300 | while IFS='|' read -r ras opts vras vopts; do echo "CX418HD|$ras|$opts|$vras|$vopts"; done >> "$T/cases"

while IFS='|' read -r model ras opts vras vopts kind; do
    if [ -n "$vopts" ] || [ "$kind" = fixed ]; then
        check "$model" "$ras" "$opts" "$vras" "$vopts"
    else
        check "$model" "$ras" "$opts"
    fi
done < "$T/cases"
echo "filter: pass=$pass fail=$fail"

# Our PPDs: rasterize each size with our PPD and check SIZE and bitmap width.
sizes_ok=0; sizes_bad=0
for m in $MODELS; do
    ppd="$OURS/$(echo "$m" | tr A-Z a-z).ppd"
    for size in $(sed -n 's/^\*PageSize \([^/]*\)\/.*/\1/p' "$ppd"); do
        # set -e must not end the run here. An empty raster fails the check below.
        cupsfilter -p "$ppd" -o PageSize="$size" -m application/vnd.cups-raster "$T/label.pdf" > "$T/s.ras" 2>/dev/null || true
        out=$(PPD="$ppd" "$NATIVE" 1 u t 1 "PageSize=$size" < "$T/s.ras" 2>/dev/null | head -c 700 | LC_ALL=C tr -d '\0' | LC_ALL=C tr '\r' ' ')
        if python3 -I - "$ppd" "$size" "$out" <<'PY'
import re, sys
ppd, size, out = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(ppd, encoding="latin-1").read()
w, h = map(float, re.search(r'^\*PaperDimension %s/[^:]*: "([^"]+)"' % size, text, re.M).group(1).split())
dpi = int(re.search(r"^\*DefaultResolution: (\d+)dpi", text, re.M).group(1))
want_size = f"SIZE {w * 25.4 / 72:.1f} mm, {h * 25.4 / 72:.1f} mm"
m = re.search(r"BITMAP 0,0,(\d+),(\d+),(?:1|16),", out)
wbytes = (round(w * dpi / 72) + 7) // 8
ok = want_size in out and m and abs(int(m.group(1)) - wbytes) <= 1
sys.exit(0 if ok else (print(f"FAIL size {ppd} {size}: want {want_size!r} wbytes~{wbytes}, got {out[:120]!r}") or 1))
PY
        then sizes_ok=$((sizes_ok + 1)); else sizes_bad=$((sizes_bad + 1)); fi
    done
done
echo "our PPD sizes: pass=$sizes_ok fail=$sizes_bad"
[ "$fail" = 0 ] && [ "$sizes_bad" = 0 ]
