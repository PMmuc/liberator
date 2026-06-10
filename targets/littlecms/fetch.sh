#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##

git clone --no-checkout https://github.com/mm2/Little-CMS.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout 21c582a594fe5279f90c0b93437c398f93bf62b0
