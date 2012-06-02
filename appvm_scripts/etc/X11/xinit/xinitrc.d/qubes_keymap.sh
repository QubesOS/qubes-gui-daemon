#!/bin/sh

QUBES_KEYMAP="`/usr/bin/xenstore-read qubes_keyboard`"
QUBES_KEYMAP="`echo -e $QUBES_KEYMAP`"
GSETTINGS_LAYOUT=`gsettings get org.gnome.libgnomekbd.keyboard layouts 2> /dev/null`
if [ $? -eq 0 ]; then
    QUBES_USER_KEYMAP=`echo $GSETTINGS_LAYOUT | tr -d "'[]"`
else
    QUBES_USER_KEYMAP=`cat $HOME/.config/qubes-keyboard-layout.rc 2> /dev/null`
fi

if [ -n "$QUBES_KEYMAP" ]; then
    echo "$QUBES_KEYMAP" | xkbcomp - $DISPLAY
fi

if [ -n "$QUBES_USER_KEYMAP" ]; then
    setxkbmap $QUBES_USER_KEYMAP
fi
