#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/cisco/openh264.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 652bdb7719f30b52b08e506645a7322ff1b2cc6f
