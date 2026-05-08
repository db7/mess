#!/bin/sh
set -e

if [ "$#" -ne 3 ]; then
	echo "usage: $0 <smolbsd-dir> <service> <payload.img>" >&2
	exit 64
fi

SMOLDIR=$1
SERVICE=$2
PAYLOAD_IMG=$3

SMOLDIR=$(readlink -f "$SMOLDIR")
PAYLOAD_IMG=$(readlink -f "$PAYLOAD_IMG")

ARCH=$(uname -m)
if [ "$ARCH" = "x86_64" ]; then
	ARCH=amd64
fi

(
	cd "$SMOLDIR"
	./startnb.sh \
		-m 1024 \
		-k kernels/netbsd-SMOL \
		-i "images/$SERVICE-$ARCH.img" \
		-l "$PAYLOAD_IMG" \
		-x "-no-reboot"
)
