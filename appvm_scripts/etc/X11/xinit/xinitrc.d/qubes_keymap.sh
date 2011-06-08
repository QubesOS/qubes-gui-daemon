#!/bin/sh

QUBES_KEYMAP="`/usr/bin/xenstore-read qubes_keyboard`"

if [ -n "$QUBES_KEYMAP" ]; then
    echo "$QUBES_KEYMAP" | xkbcomp - $DISPLAY
fi
