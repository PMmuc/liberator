#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/madler/zlib  \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout da607da739fa6047df13e66a2af6b8bec7c2a498
