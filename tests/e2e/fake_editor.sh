#!/bin/bash
# Fake editor for the e2e tests: replaces the contents of file $1 with $FAKE_EDIT_TO.
printf '%s\n' "$FAKE_EDIT_TO" > "$1"
