#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <smolbsd.img>" >&2
  exit 64
fi

IMG=$(readlink -f "$1")

if [ ! -f "$IMG" ]; then
  echo "image not found: $IMG" >&2
  exit 66
fi

modprobe loop >/dev/null 2>&1 || true
modprobe ufs >/dev/null 2>&1 || true

MNT=$(mktemp -d)

cleanup() {
  set +e
  if mountpoint -q "$MNT"; then
    umount "$MNT"
  fi
  rm -rf "$MNT"
}

trap cleanup EXIT INT TERM HUP

if ! mount -t ufs -o loop,ufstype=ufs2 "$IMG" "$MNT" 2>/dev/null; then
  if ! mount -t ffs -o loop "$IMG" "$MNT" 2>/dev/null; then
    if ! mount -o loop "$IMG" "$MNT" 2>/dev/null; then
      echo "failed to mount $IMG" >&2
      exit 70
    fi
  fi
fi

LOG="$MNT/tests.log"
FLAG="$MNT/tests.ok"

if [ -f "$LOG" ]; then
  cp "$LOG" ./tests.log
  echo "===== smolBSD test log ====="
  cat tests.log
  echo "===== end log ====="
else
  echo "no tests.log present in image" >&2
fi

if [ -f "$FLAG" ]; then
  exit 0
fi

exit 1
