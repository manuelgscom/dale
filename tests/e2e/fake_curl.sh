#!/bin/bash
# Fake curl for the e2e tests: logs the call to $FAKE_CURL_LOG and copies $FAKE_CURL_SRC to
# the -o file. Never touches the network.
printf '%s\n' "$*" >> "$FAKE_CURL_LOG"
out=
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift ;;
    esac
    shift
done
[ -n "$out" ] || exit 2
cat "$FAKE_CURL_SRC" > "$out"
