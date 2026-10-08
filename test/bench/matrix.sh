#!/bin/bash
# The desktop benchmark over several screen sizes, window sizes and thresholds of the DMA (see bench.sh); about 75 s a measure.
#
#   matrix.sh <outfile> [min ...]
#
# Without thresholds: DMA off (2147483647), 8192 and 32768. Screens 1280x720, 1920x1080 and 1920x1200, each with a big window and a 300x200 one.
# Results: parse.py <outfile> makes the tables.

HERE=$(dirname "$0")
OUT=${1:?usage: matrix.sh <outfile> [min ...]}
shift
MINS=${*:-2147483647 8192 32768}

for res in "1280 720 900 600" "1920 1080 1400 800" "1920 1200 1400 900"; do
  set -- $res
  sw=$1; sh=$2; bw=$3; bh=$4
  for win in "$bw $bh" "300 200"; do
    for min in $MINS; do
      bash "$HERE/bench.sh" "$sw" "$sh" ${win% *} ${win#* } "$min" "dma-min-$min" "$OUT"
      sleep 2
    done
  done
done

echo "DONE $OUT"
