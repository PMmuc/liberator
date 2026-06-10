#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/sqlite/sqlite.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 189e44dfecdc7868bb860dfb5d98eab371318c37
