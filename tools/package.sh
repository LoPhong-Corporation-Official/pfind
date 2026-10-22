#!/usr/bin/env bash
# Builds a release archive for one platform.
#
# Usage: tools/package.sh <path-to-pfind-binary> <target-name> <tar.gz|zip> [label]
#   e.g. tools/package.sh build/pfind linux-x86_64 tar.gz v1.5.0
#
# Creates dist/pfind-<label>-<target>.<ext> and a matching .sha256 file. The archive contains the
# binary, README.md and the shell completion scripts.
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: $0 <binary> <target-name> <tar.gz|zip> [label]" >&2
    exit 2
fi

exe=$1
target=$2
kind=$3
label=${4:-dev}

case "$kind" in
    tar.gz|zip) ;;
    *) echo "error: archive kind must be tar.gz or zip, got '$kind'" >&2; exit 2 ;;
esac

if [ ! -f "$exe" ]; then
    echo "error: binary not found: $exe" >&2
    exit 1
fi

name="pfind-${label}-${target}"
archive="${name}.${kind}"
out=dist

rm -rf "${out:?}/${name}" "${out}/${archive}" "${out}/${archive}.sha256"
mkdir -p "${out}/${name}/completions"

cp "$exe" "${out}/${name}/"
cp README.md "${out}/${name}/"
"$exe" --completion bash       > "${out}/${name}/completions/pfind.bash"
"$exe" --completion zsh        > "${out}/${name}/completions/_pfind"
"$exe" --completion powershell > "${out}/${name}/completions/pfind.ps1"

(
    cd "$out"
    if [ "$kind" = zip ]; then
        cmake -E tar cf "$archive" --format=zip "$name"
    else
        cmake -E tar czf "$archive" "$name"
    fi

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$archive" > "${archive}.sha256"
    else
        shasum -a 256 "$archive" > "${archive}.sha256"
    fi

    rm -rf "$name"
)

echo "${out}/${archive}"
