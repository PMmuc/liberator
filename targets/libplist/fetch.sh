#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/libimobiledevice/libplist.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 02cf35bb445ad1a6ed6180f78cfb6528a1e36c19
