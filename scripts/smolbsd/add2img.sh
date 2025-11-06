#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <smolbsd.img> <workspace>" >&2
  exit 64
fi

IMG=$(readlink -f "$1")
WORKDIR=$(readlink -f "$2")

if [ ! -f "$IMG" ]; then
  echo "image not found: $IMG" >&2
  exit 66
fi

if [ ! -d "$WORKDIR" ]; then
  echo "workspace not found: $WORKDIR" >&2
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

rm -rf "$MNT/workspace"
mkdir -p "$MNT/workspace"
cp -R "$WORKDIR"/. "$MNT/workspace/"

cat >"$MNT/test.sh" <<'EOF'
#!/bin/sh
set -ex
rm -f /tests.ok /tests.log
exec >/tests.log 2>&1
export HOME=/root
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin
cd /workspace
make clean || true
make || reboot
make test || reboot
touch /tests.ok
reboot
EOF
chmod 755 "$MNT/test.sh"

cat >"$MNT/etc/rc" <<'EOF'
#!/bin/sh
export HOME=/root
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/pkg/bin
umask 022
mount -o rw /
sh /test.sh
EOF
chmod 755 "$MNT/etc/rc"

sync
