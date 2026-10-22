#!/usr/bin/env bash
# Fails unless the git tag (e.g. v1.5.0) matches the version in CMakeLists.txt and the binary.
#
# Usage: tools/check-version.sh <tag> [path-to-pfind-binary]
set -euo pipefail

tag=${1:?usage: $0 <tag> [binary]}
cmake_version=$(sed -n 's/^project(pfind VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)

if [ -z "$cmake_version" ]; then
    echo "error: could not read the version from CMakeLists.txt" >&2
    exit 1
fi

if [ "v${cmake_version}" != "$tag" ]; then
    echo "error: tag '$tag' does not match the version in CMakeLists.txt (${cmake_version})" >&2
    exit 1
fi

if [ "$#" -ge 2 ]; then
    reported=$("$2" --version)
    if [ "$reported" != "pfind ${cmake_version}" ]; then
        echo "error: binary reports '$reported', expected 'pfind ${cmake_version}'" >&2
        exit 1
    fi
fi

echo "version ${cmake_version} OK"
