#!/bin/bash
# One measure of the desktop benchmark: opens a public screen of the size asked for (brcm-dma-screen), sets the threshold of the DMA of VideoCore.card
# at run time (vcdmaset), resets the counters of the resource, runs vcwin (SCROLL FILL LMOVE LSIZE MIX, 2 s each: the counters of the 68k and the
# cycles an operation are only read for 2 s or less) and reads the counters again.
#
#   bench.sh <screenW> <screenH> <winW> <winH> <min> <label> <outfile>
#
# <min> is the threshold in bytes of the DMA of BlitRect; 2147483647 keeps everything off the DMA.
# Needs squirt (squirt_cli.exe) and, on the Amiga, brcm-dma-screen and brcm-dma-info (build/install/tools/), vcwin and vcdmaset (VideoCore.card, bench/).
# Variables: SQUIRT (folder of squirt_cli.exe), AMIGA (address), TOOLS (folder of the brcm-dma tools on the Amiga), BENCH (folder of vcwin and vcdmaset).

SQUIRT=${SQUIRT:-/c/Developers/Herald/squirt}
AMIGA=${AMIGA:-192.168.1.93}
TOOLS=${TOOLS:-DATA:SourcesEmu68/brcm-dma.resource/tools}
BENCH=${BENCH:-DATA:SourcesEmu68/VideoCore.card/bench}

SW=$1; SH=$2; WW=$3; WH=$4; MIN=$5; LABEL=$6; OUT=$7

{
echo "### $LABEL screen ${SW}x${SH} window ${WW}x${WH} min $MIN"
printf 'cd %s\nrun >NIL: brcm-dma-screen %s %s SECS 120\nwait 3\n%s/vcdmaset.v1 %s\n%s/brcm-dma-info RESET\n%s/vcwin.v9 SCROLL FILL LMOVE LSIZE MIX W %s H %s SECS 2\n%s/brcm-dma-info\necho >T:bdma.stop\nwait 3\nC:vcmailbox 0x30006 8 0 0\nendcli\n' \
  "$TOOLS" "$SW" "$SH" "$BENCH" "$MIN" "$TOOLS" "$BENCH" "$WW" "$WH" "$TOOLS" \
  | timeout 150 "$SQUIRT/squirt_cli.exe" "$AMIGA" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' \
  | grep -E 'ops/s|ACTIVE|^jobs|^sizes|^queue|screen [0-9]|0x0000000|rror|fail' | grep -v 'OS321\|^1\.'
} >> "$OUT"
