#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone https://chromium.googlesource.com/webm/libvpx \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 1024874c5919305883187e2953de8fcb4c3d7fa6