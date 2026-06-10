#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://android.googlesource.com/platform/external/pthreadpool \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 3278138cd43f4d81aed2a406680f2dc328492d2e