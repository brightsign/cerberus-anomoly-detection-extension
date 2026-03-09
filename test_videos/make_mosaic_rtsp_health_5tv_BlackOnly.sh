#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# make_mosaic_rtsp_health_5tv.sh
#
# Create a single RTSP "video wall" mosaic stream with 5 TV tiles from ONE input
# video (looped), plus synthetic tiles to simulate:
#   - BLACK / BLANK (tv2: black overlay during a time window)
#   - TV OFF (tv3: always black)
#   - NO SIGNAL / WRONG INPUT / INPUT MENU (tv4/tv5: synthetic OSD screens)
#
# Layout (3x2):
#   Row 1: tv1_clean | tv2_black_window | tv4_no_signal
#   Row 2: tv3_tv_off | tv5_hdmi2_menu  | blank
#
# Output mosaic size: 1278x720 (each tile 426x360)
#
# Dependencies:
#   - ffmpeg
#   - An RTSP server (MediaMTX recommended)
#
# Example:
#   docker run --rm -it -p 8554:8554 bluenviron/mediamtx:latest
#   ./make_mosaic_rtsp_health_5tv.sh Anomaly_Wallmart.mp4 rtsp://127.0.0.1:8554/wall
#
# Tune using environment variables:
#   TILE_W=426 TILE_H=360 FPS=30
#   BLACK_AT=8 BLACK_DUR=4
#   NOSIGNAL_TEXT="NO SIGNAL"
#   HDMI_TEXT="HDMI2"
# =============================================================================

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <input_video.mp4> <rtsp_url>"
  echo "Example: $0 Anomaly_Wallmart.mp4 rtsp://127.0.0.1:8554/wall"
  exit 1
fi

IN="$1"
OUT_RTSP="$2"

TILE_W="${TILE_W:-426}"
TILE_H="${TILE_H:-360}"
FPS="${FPS:-30}"

# Black-window (tv2) parameters (seconds)
BLACK_AT="${BLACK_AT:-8}"
BLACK_DUR="${BLACK_DUR:-4}"

# OSD text
NOSIGNAL_TEXT="${NOSIGNAL_TEXT:-NO SIGNAL}"
NOSIGNAL_SUBTEXT="${NOSIGNAL_SUBTEXT:-Check input source}"
HDMI_TEXT="${HDMI_TEXT:-HDMI2}"
HDMI_SUBTEXT="${HDMI_SUBTEXT:-No device detected}"

# Encoding params (low-latency)
PRESET="${PRESET:-veryfast}"
GOP="${GOP:-30}"

# If your ffmpeg has drawtext but not default font discovery, set FONTFILE
# e.g.: FONTFILE=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
FONTFILE_OPT=""
if [[ -n "${FONTFILE:-}" ]]; then
  FONTFILE_OPT=":fontfile=${FONTFILE}"
fi

echo "Input:      $IN"
echo "RTSP out:   $OUT_RTSP"
echo "Tile:       ${TILE_W}x${TILE_H} @ ${FPS}fps"
echo "tv2 BLACK:  at ${BLACK_AT}s for ${BLACK_DUR}s"
echo "OSD:        tv4='${NOSIGNAL_TEXT}', tv5='${HDMI_TEXT}'"
echo

# Notes:
# - We loop the input indefinitely with -stream_loop -1
# - We run in realtime pacing with -re
# - tv1 = clean scaled input
# - tv2 = clean scaled input with full-frame black overlay enabled between t=[BLACK_AT, BLACK_AT+BLACK_DUR]
# - tv3 = pure black (TV off)
# - tv4 = "NO SIGNAL" OSD screen (synthetic)
# - tv5 = "HDMI2" input menu (synthetic)
# - tile6 = blank filler

ffmpeg -hide_banner -loglevel info \
  -re -stream_loop -1 -i "$IN" \
  -f lavfi -i "color=c=black:s=${TILE_W}x${TILE_H}:r=${FPS}" \
  -f lavfi -i "color=c=gray:s=${TILE_W}x${TILE_H}:r=${FPS}" \
  -f lavfi -i "color=c=navy:s=${TILE_W}x${TILE_H}:r=${FPS}" \
  -f lavfi -i "color=c=black:s=${TILE_W}x${TILE_H}:r=${FPS}" \
  -filter_complex "\
    [0:v]fps=${FPS},scale=${TILE_W}:${TILE_H},format=yuv420p[v0]; \
    [v0]split=2[tv1][tv2src]; \
    [tv2src]drawbox=x=0:y=0:w=iw:h=ih:color=black@1.0:t=fill:enable='between(t,${BLACK_AT},${BLACK_AT}+${BLACK_DUR})'[tv2]; \
    [1:v]format=yuv420p[tv3]; \
    [2:v]format=yuv420p, \
         drawtext=text='${NOSIGNAL_TEXT}':x=(w-text_w)/2:y=(h-text_h)/2-20:fontsize=42:fontcolor=white${FONTFILE_OPT}, \
         drawtext=text='${NOSIGNAL_SUBTEXT}':x=(w-text_w)/2:y=(h-text_h)/2+30:fontsize=24:fontcolor=white${FONTFILE_OPT}, \
         drawbox=x=0:y=0:w=iw:h=ih:color=black@0.15:t=fill \
         [tv4]; \
    [3:v]format=yuv420p, \
         drawbox=x=20:y=20:w=iw-40:h=70:color=black@0.35:t=fill, \
         drawtext=text='Input':x=35:y=40:fontsize=28:fontcolor=white${FONTFILE_OPT}, \
         drawbox=x=20:y=110:w=iw-40:h=200:color=black@0.25:t=fill, \
         drawtext=text='${HDMI_TEXT}':x=35:y=140:fontsize=42:fontcolor=white${FONTFILE_OPT}, \
         drawtext=text='${HDMI_SUBTEXT}':x=35:y=200:fontsize=24:fontcolor=white${FONTFILE_OPT} \
         [tv5]; \
    [4:v]format=yuv420p[blank]; \
    [tv1][tv2][tv4][tv3][tv5][blank]xstack=inputs=6:layout=\
0_0|${TILE_W}_0|$((${TILE_W}*2))_0|0_${TILE_H}|${TILE_W}_${TILE_H}|$((${TILE_W}*2))_${TILE_H}:fill=black[outv] \
  " \
  -map "[outv]" -an \
  -c:v libx264 -preset "${PRESET}" -tune zerolatency -pix_fmt yuv420p \
  -g "${GOP}" -keyint_min "${GOP}" -bf 0 \
  -f rtsp -rtsp_transport tcp "$OUT_RTSP"

