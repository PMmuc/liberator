#!/bin/bash
#
# Pre-requirements:
# - env TARGET: path to target work dir
#
# Idempotent: safe to re-run. Reuses an existing checkout and only fetches the
# pinned commit if a previous (older) clone predates it.
set -e

REPO="$TARGET/repo"
URL="https://github.com/cisco/openh264.git"
COMMIT="652bdb7719f30b52b08e506645a7322ff1b2cc6f"

if [ ! -d "$REPO/.git" ]; then
  git clone --no-checkout "$URL" "$REPO"
fi

# Make sure the pinned commit exists locally; fetch it if not.
if ! git -C "$REPO" cat-file -e "${COMMIT}^{commit}" 2>/dev/null; then
  git -C "$REPO" fetch --tags origin || git -C "$REPO" fetch origin "$COMMIT" || true
fi

git -C "$REPO" checkout -f --detach "$COMMIT"
