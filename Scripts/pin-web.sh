#!/usr/bin/env bash
# Scripts/pin-web.sh <curl-version> <mbedtls-version>: prints the cmake/FMinusPins.cmake lines for a libcurl release
# (e.g. 8.23.0) and a Mbed TLS release (e.g. 4.1.1), the web library's dependencies (cmake/FMinusWeb.cmake). libcurl's
# tarball is downloaded from curl.se into a temporary directory and hashed (its release page on GitHub shows the same
# digest); Mbed TLS's digest is read from its GitHub release's asset list, as Scripts/pin-llvm.sh reads LLVM's (nothing
# of it is downloaded). Bump both on their security releases. Needs curl, python3, and sha256sum or shasum.
set -euo pipefail
curl_version="${1:?usage: Scripts/pin-web.sh <curl-version> <mbedtls-version>, e.g. 8.23.0 4.1.1}"
mbedtls_version="${2:?usage: Scripts/pin-web.sh <curl-version> <mbedtls-version>, e.g. 8.23.0 4.1.1}"

hash() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
archive="${work}/curl-${curl_version}.tar.xz"
curl -fsSL -o "${archive}" "https://curl.se/download/curl-${curl_version}.tar.xz"
size="$(wc -c < "${archive}" | tr -d ' ')"
echo "set(FMINUS_CURL_VERSION \"${curl_version}\")"
echo "set(FMINUS_CURL_SHA256 \"$(hash "${archive}")\")   # $(python3 -c "print(f'{${size}:,}')") bytes"

asset="mbedtls-${mbedtls_version}.tar.bz2"
curl -fsSL "https://api.github.com/repos/Mbed-TLS/mbedtls/releases/tags/mbedtls-${mbedtls_version}" |
python3 -c '
import json, sys
asset = sys.argv[1]
release = json.load(sys.stdin)
for a in release.get("assets", []):
    if a["name"] == asset:
        digest = a.get("digest") or ""
        if not digest.startswith("sha256:"):
            sys.exit(f"{asset}: the release lists no SHA-256 digest")
        size = a["size"]
        print(f"set(FMINUS_MBEDTLS_VERSION \"{sys.argv[2]}\")")
        print(f"set(FMINUS_MBEDTLS_SHA256 \"{digest[7:]}\")   # {size:,} bytes")
        break
else:
    sys.exit(f"{asset}: not in the release")
' "${asset}" "${mbedtls_version}"
