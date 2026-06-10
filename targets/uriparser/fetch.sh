#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/uriparser/uriparser.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 9b2bed92f5deecf740819f9bf27724bee2fe9c12