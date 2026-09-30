#!/usr/bin/env bash
# Shortcut for nfsmw/build-msys2.sh (same options: --check, --install-packages, --setup-rust).
# From the Mesa folder in an MSYS2 shell:  bash build-nfsmw.sh
exec bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/nfsmw/build-msys2.sh" "$@"
