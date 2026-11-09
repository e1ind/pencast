#!/bin/sh
# Rebuild the test corpus. Videos are regenerated only if missing.
set -e
cd "$(dirname "$0")"
[ -f clip720p30.mp4 ] || ffmpeg -loglevel error -y -f lavfi -i testsrc=size=1280x720:rate=30:duration=10 \
  -f lavfi -i "aevalsrc=0.15*sin(2*PI*1000*t)*lt(mod(t\\,1)\\,0.05):d=10" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest -movflags +faststart clip720p30.mp4
[ -f clip1080p60.mp4 ] || ffmpeg -loglevel error -y -f lavfi -i testsrc=size=1920x1080:rate=60:duration=20 \
  -c:v libx264 -pix_fmt yuv420p -an -movflags +faststart clip1080p60.mp4
[ -f poster720p30.png ] || ffmpeg -loglevel error -y -ss 1 -i clip720p30.mp4 -frames:v 1 poster720p30.png
[ -f poster1080p60.png ] || ffmpeg -loglevel error -y -ss 1 -i clip1080p60.mp4 -frames:v 1 poster1080p60.png
for f in media_test stress_100; do latexmk -pdf -interaction=nonstopmode -quiet $f.tex; done
latexmk -c >/dev/null
