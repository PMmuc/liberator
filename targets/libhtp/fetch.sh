#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/OISF/libhtp.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 16e23594c61f7719f8cb1cd19ca69bbafb37a0eb