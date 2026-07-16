#!/bin/bash
#
# Pre-requirements:
# - env TARGET: path to target work dir
#
# Idempotent: safe to re-run. Reuses an existing checkout and only fetches the
# pinned commit if a previous (older) clone predates it.
set -e

REPO="$TARGET/repo"
URL="https://github.com/c-ares/c-ares.git"
COMMIT="3ac47ee46edd8ea40370222f91613fc16c434853"

if [ ! -d "$REPO/.git" ]; then
  git clone --no-checkout "$URL" "$REPO"
fi

# Make sure the pinned commit exists locally; fetch it if not.
if ! git -C "$REPO" cat-file -e "${COMMIT}^{commit}" 2>/dev/null; then
  git -C "$REPO" fetch --tags origin || git -C "$REPO" fetch origin "$COMMIT" || true
fi

git -C "$REPO" checkout -f --detach "$COMMIT"
