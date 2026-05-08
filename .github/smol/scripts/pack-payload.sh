#!/bin/sh
set -e

if [ "$#" -lt 2 ]; then
	echo "usage: $0 <payload.img> <file> [file]..." >&2
	exit 64
fi

IMG=$1
IMG=$(readlink -f "$IMG")
shift

MTOOLSRC=$HOME/.mtoolsrc

cleanup()
{
	set +e
	if [ -f "$MTOOLSRC.bak" ]; then
		mv "$MTOOLSRC.bak" "$MTOOLSRC"
	else
		rm -f "$MTOOLSRC"
	fi
}

trap cleanup EXIT INT TERM HUP

dd if=/dev/zero of="$IMG" bs=1M count=16
mkfs.fat -F 16 "$IMG"

if [ -f "$MTOOLSRC" ]; then
	cp "$MTOOLSRC" "$MTOOLSRC.bak"
fi
echo "drive x: file=\"$IMG\"" > "$MTOOLSRC"

for file in "$@"; do
	echo "copying $file"
	mcopy "$file" x:/
done
