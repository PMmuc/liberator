#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/GNOME/libxml2.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout c94eb0210183b9d7cb43f8e7fddc6be55843ef49