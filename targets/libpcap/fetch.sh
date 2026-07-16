#!/bin/bash
#
# Pre-requirements:
# - env TARGET: path to target work dir
#
# Idempotent: safe to re-run. Reuses an existing checkout and only fetches the
# pinned commit if a previous (older) clone predates it.
set -e

REPO="$TARGET/repo"
URL="https://github.com/the-tcpdump-group/libpcap.git"
COMMIT="a999701dca5c873779281938baee6bc185a8d4dc"

if [ ! -d "$REPO/.git" ]; then
  git clone --no-checkout "$URL" "$REPO"
fi

# Make sure the pinned commit exists locally; fetch it if not.
if ! git -C "$REPO" cat-file -e "${COMMIT}^{commit}" 2>/dev/null; then
  git -C "$REPO" fetch --tags origin || git -C "$REPO" fetch origin "$COMMIT" || true
fi

git -C "$REPO" checkout -f --detach "$COMMIT"
