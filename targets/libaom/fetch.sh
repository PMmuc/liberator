#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##
git clone --no-checkout https://aomedia.googlesource.com/aom \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 92d4c37fbdd08944a0e721bbaeb13318f10aebb0
