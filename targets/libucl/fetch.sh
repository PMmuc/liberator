#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/vstakhov/libucl.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 058286f4f85e2a66130e8bdaddf402d9c78d259c
