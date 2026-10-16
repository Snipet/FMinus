#!/usr/bin/env bash
# Scripts/pin-llvm.sh <version>: prints the cmake/FMinusPins.cmake lines for an LLVM release (e.g. 23.1.3), read from
# the GitHub release's asset list (size and SHA-256 digest; nothing is downloaded). Needs curl and python3.
set -euo pipefail
version="${1:?usage: Scripts/pin-llvm.sh <version>, e.g. 23.1.3}"
asset="llvm-project-${version}.src.tar.xz"
curl -fsSL "https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-${version}" |
python3 -c '
import json, sys
asset = sys.argv[1]
release = json.load(sys.stdin)
for a in release.get("assets", []):
    if a["name"] == asset:
        digest = a.get("digest") or ""
        if not digest.startswith("sha256:"):
            sys.exit(f"{asset}: the release lists no SHA-256 digest")
        print(f"set(FMINUS_LLVM_VERSION \"{sys.argv[2]}\")")
        size = a["size"]
        print(f"set(FMINUS_LLVM_SHA256 \"{digest[7:]}\")   # {size:,} bytes")
        break
else:
    sys.exit(f"{asset}: not in the release")
' "${asset}" "${version}"
