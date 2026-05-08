#!/bin/sh
set -e

if [ "$#" -ne 2 ]; then
	echo "usage: $0 <payload.img> <unpack-dir>" >&2
	exit 64
fi

IMG=$1
DIR=$2
IMG=$(readlink -f "$IMG")
mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)

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

if [ -f "$MTOOLSRC" ]; then
	cp "$MTOOLSRC" "$MTOOLSRC.bak"
fi
echo "drive x: file=\"$IMG\"" > "$MTOOLSRC"

mcopy 'x:/*' "$DIR/"
