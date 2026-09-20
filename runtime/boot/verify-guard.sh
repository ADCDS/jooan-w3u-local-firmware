#!/bin/sh
# Packaging installs this helper under shared/, outside the A/B runtime slots.
. /run/jooan-local/controller/boot/common.sh || exit 127
jl_verify_archive /run/jooan-local/controller/shared/libjooan_guard.so \
    /run/jooan-local/controller/shared/guard.md5 || exit 127
exit 0
