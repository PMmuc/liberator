#!/bin/bash

##
# Pre-requirements:
# - env TARGET: path to target work dir
##
git clone --no-checkout https://github.com/the-tcpdump-group/libpcap.git \
    "$TARGET/repo"
git -C "$TARGET/repo" checkout a999701dca5c873779281938baee6bc185a8d4dc
