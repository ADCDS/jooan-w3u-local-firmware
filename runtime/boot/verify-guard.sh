#!/bin/sh
# Packaging installs this helper under shared/, outside the A/B runtime slots.
. /opt/custom/jooan-local/boot/common.sh || exit 127
jl_verify_archive /opt/custom/jooan-local/shared/libjooan_guard.so \
    /opt/custom/jooan-local/shared/guard.sha256 || exit 127
exit 0
