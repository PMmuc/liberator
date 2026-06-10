#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://gitlab.com/libtiff/libtiff.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 5fe20d0e9aba49a6a350ed533459d1505203838f