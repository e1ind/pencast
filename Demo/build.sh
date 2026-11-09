#!/bin/sh
# Build the PenCast showcase deck: media first (skipped if present), then LuaLaTeX.
set -e
cd "$(dirname "$0")"
python3 make_media.py
latexmk -lualatex -interaction=nonstopmode -quiet PenCast_Demo.tex
latexmk -c >/dev/null
