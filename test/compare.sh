#!/bin/sh
# Compares raster-tspl output with the vendor's x86 filter (run under Rosetta).
# Usage: test/compare.sh [vendor-filter] [ppd]   (run from the repo root on a Mac)
set -e
VENDOR=${1:-/usr/libexec/cups/filter/raster-tspl}
# Use the vendor backup when the native filter has been installed in its place.
[ -z "$1" ] && [ -x "$VENDOR.x86_64-vendor" ] && VENDOR=$VENDOR.x86_64-vendor
PPDGZ=${2:-/Library/Printers/PPDs/Contents/Resources/CX418.TSPL.ppd.gz}
NATIVE=$(pwd)/raster-tspl
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

gunzip -c "$PPDGZ" > "$T/cx418.ppd"
python3 -I "$(dirname "$0")/make_label.py" "$T/label.pdf"
cupsfilter -p "$T/cx418.ppd" -m application/vnd.cups-raster "$T/label.pdf" > "$T/gray8.ras" 2>/dev/null
cupsfilter -p "$T/cx418.ppd" -o ColorOption=GrayScale -m application/vnd.cups-raster "$T/label.pdf" > "$T/gray1.ras" 2>/dev/null
cupsfilter -p "$T/cx418.ppd" -o PageSize=w50h25 -m application/vnd.cups-raster "$T/label.pdf" > "$T/small.ras" 2>/dev/null
# A CUPS raster stream is one sync word followed by pages, so append page 1 again.
python3 -I -c 'import sys; d=open(sys.argv[1],"rb").read(); open(sys.argv[2],"wb").write(d+d[4:])' "$T/gray8.ras" "$T/twopage.ras"

pass=0; fail=0
while IFS='|' read -r ras opts; do
    a=$(PPD="$T/cx418.ppd" "$VENDOR" 1 u t 1 "$opts" < "$T/$ras" 2>/dev/null | shasum)
    b=$(PPD="$T/cx418.ppd" "$NATIVE" 1 u t 1 "$opts" < "$T/$ras" 2>/dev/null | shasum)
    if [ "$a" = "$b" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $ras [$opts]"; fi
done <<CASES
gray8.ras|
gray1.ras|
twopage.ras|PrintDarkness=8
small.ras|PageSize=w50h25
small.ras|PageSize=w50h25 PaperType=2 PrintSpeed=2
gray8.ras|PrintDarkness=10 PrintSpeed=4 PaperType=1 ShiftMove=2 FowardOffset=-3 MediaMethod=1
$(for s in -12 -11 -3 -1 0 1 5 11 12; do echo "gray8.ras|ShiftMove=$s"; done)
gray8.ras|FowardOffset=30 PrintDarkness=0
gray8.ras|FowardOffset=-30 PrintDarkness=15 PrintSpeed=6
gray8.ras|PaperType=3 PostAction=None
gray8.ras|PaperType=2
gray8.ras|MirrorImage=True
gray8.ras|NegativeImage=True
gray8.ras|Orientation=2
gray8.ras|Orientation=2 MirrorImage=True NegativeImage=True
gray1.ras|Orientation=2 NegativeImage=True
twopage.ras|Orientation=2 PaperType=1
CASES
echo "pass=$pass fail=$fail"
[ "$fail" = 0 ]
