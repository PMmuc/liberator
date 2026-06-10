#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##
git clone --no-checkout https://github.com/c-ares/c-ares.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 3ac47ee46edd8ea40370222f91613fc16c434853
