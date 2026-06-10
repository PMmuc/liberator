#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/davea42/libdwarf-code.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout b5ef10c9df0f494596fd9d31e19048a3ed5f28ba
