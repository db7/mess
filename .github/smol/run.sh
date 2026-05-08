#!/bin/sh
set -e

fail()
{
	exit 1
}

echo "extract project"
mkdir -p /project
tar -xf /mnt/project.tar -C /project || fail

(
	cd /project
	rm -f /mnt/tests.ok /mnt/tests.log
	exec >/mnt/tests.log 2>&1
	if [ -f /mnt/tikl-0.4.2.tar.gz ]; then
		mkdir -p deps
		cp /mnt/tikl-0.4.2.tar.gz deps/
	fi
	echo "build"
	make clean || true
	make || fail
	echo "test"
	make tikl-fetch-build || fail
	make test || fail
	touch /mnt/tests.ok
)
