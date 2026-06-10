#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##
git clone --no-checkout https://github.com/DaveGamble/cJSON.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout c859b25da02955fef659d658b8f324b5cde87be3
