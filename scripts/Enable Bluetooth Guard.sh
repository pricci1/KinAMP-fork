#!/bin/sh
# Name: Enable Bluetooth Guard
# Author: KoReadalong
# Default Scriptlet FBInk output: no hooks, no boot integration.

printf 'Bluetooth guard\n\n'
G=/var/local/kinamp-bt-guard/bt-guard
if [ ! -x "$G" ]; then
    echo 'The Bluetooth guard is not installed.'
    sleep 6
    exit 1
fi
if "$G" enable; then
    echo 'You can now open KOReader and listen.'
    sleep 6
else
    echo 'Not enabled. Stop playback before trying again.'
    echo 'If the session is stalled, use the documented recovery.'
    sleep 8
    exit 1
fi
