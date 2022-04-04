#!/bin/sh
echo "compress-then-send script"
number="$1"
callerid="$2"
filename="$3"
lame "$filename"

filename="${filename%.*}"

./mail.py "$number" "$callerid" "$filename.mp3"
