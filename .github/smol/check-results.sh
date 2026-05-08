#!/bin/sh
set -e

if [ "$#" -ne 1 ]; then
	echo "usage: $0 <results-dir>" >&2
	exit 64
fi

DIR=$(readlink -f "$1")

(
	cd "$DIR"
	LOG=tests.log
	OK=tests.ok
	if [ ! -f "$LOG" ] && [ -f TESTS.LOG ]; then
		LOG=TESTS.LOG
	fi
	if [ ! -f "$OK" ] && [ -f TESTS.OK ]; then
		OK=TESTS.OK
	fi

	echo "===== smolBSD test log ====="
	if [ -f "$LOG" ]; then
		cat "$LOG"
	else
		echo "missing tests.log" >&2
	fi
	echo "===== end log ====="

	if [ -f "$OK" ]; then
		exit 0
	fi
	exit 1
)
