#!/usr/bin/env bash
# Download the latest libsodium release, check it really came from the
# libsodium project, build it as a static library, and publish it to a
# CheckMake repo as the "sodium" package.
#
# Usage: scripts/package-libsodium.sh [options]
#   --repo DIR        repo folder to publish to     (default: ~/.checkmake)
#   --key FILE        CheckMake signing key          (default: ~/.config/checkmake/signing.key)
#   --checkmake PATH  checkmake binary               (default: ./checkmake next to this repo)
#   --version X.Y.Z   package this release instead of the latest
#   --rebuild         package again even if this version is already in the repo
#                     (publishes the next revision, e.g. 1.0.22-3)
#   --check           run libsodium's test suite before packaging (slower)
#   -j N              parallel build jobs            (default: CPU count)
#
# Environment: CC picks the C compiler (default: gcc). The package's
# platform comes from "$CC -dumpmachine", e.g. x86_64-linux-gnu.
set -euo pipefail

# libsodium's release signing key, as published in its installation docs:
# https://doc.libsodium.org/installation ("Integrity checking")
LIBSODIUM_MINISIGN_KEY="RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3"
RELEASES_URL="https://download.libsodium.org/libsodium/releases"

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$HOME/.checkmake"
KEY="$HOME/.config/checkmake/signing.key"
CHECKMAKE="$PROJECT_DIR/checkmake"
VERSION=""
REBUILD=0
CHECK=0
JOBS="$(nproc)"
CC="${CC:-gcc}"

die() { echo "error: $*" >&2; exit 1; }
step() { echo "==> $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO="$2"; shift 2 ;;
        --key) KEY="$2"; shift 2 ;;
        --checkmake) CHECKMAKE="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --rebuild) REBUILD=1; shift ;;
        --check) CHECK=1; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option '$1' (see --help)" ;;
    esac
done

for tool in curl minisign tar make "$CC"; do
    command -v "$tool" >/dev/null || die "'$tool' is required but not installed"
done
[ -x "$CHECKMAKE" ] || die "checkmake not found at $CHECKMAKE (build it, or pass --checkmake)"
[ -f "$KEY" ] || die "signing key not found at $KEY (pass --key)"

# --- Platform -------------------------------------------------------------
# libsodium is a C library, so it only needs the C ABI: arch-linux-libc.
triple="$("$CC" -dumpmachine)"
arch="${triple%%-*}"
case "$triple" in
    *linux*musl*) libc=musl ;;
    *linux*gnu*) libc=gnu ;;
    *) die "unsupported target '$triple' (need Linux with glibc or musl)" ;;
esac
PLATFORM="$arch-linux-$libc"

# --- Which release ----------------------------------------------------------
if [ -z "$VERSION" ]; then
    step "Finding the latest libsodium release"
    VERSION="$(curl -fsSL "$RELEASES_URL/" |
        grep -oE 'libsodium-[0-9]+\.[0-9]+\.[0-9]+\.tar\.gz"' |
        grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -uV | tail -1)"
    [ -n "$VERSION" ] || die "couldn't find any releases at $RELEASES_URL"
fi
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "bad version '$VERSION' (expected X.Y.Z)"
echo "    libsodium $VERSION for $PLATFORM"

# --- Package revision -------------------------------------------------------
# Published packages can't change, so each repackaging of the same upstream
# version gets the next revision: 1.0.22-1, 1.0.22-2, ...
last_rev=-1
if [ -f "$REPO/manifest" ]; then
    while read -r name ver plat _; do
        [ "$name" = sodium ] && [ "$plat" = "$PLATFORM" ] || continue
        if [ "$ver" = "$VERSION" ]; then
            rev=0
        elif [[ "$ver" =~ ^${VERSION//./\\.}-([0-9]+)$ ]]; then
            rev="${BASH_REMATCH[1]}"
        else
            continue
        fi
        [ "$rev" -gt "$last_rev" ] && last_rev="$rev"
    done < <(grep -v '^#' "$REPO/manifest")
fi
if [ "$last_rev" -ge 0 ] && [ "$REBUILD" -eq 0 ]; then
    shown="$VERSION"; [ "$last_rev" -gt 0 ] && shown="$VERSION-$last_rev"
    echo "sodium $shown for $PLATFORM is already in $REPO; nothing to do."
    echo "(Use --rebuild to publish a new revision anyway.)"
    exit 0
fi
PKG_VERSION="$VERSION-$((last_rev < 1 ? 1 : last_rev + 1))"

# --- Download and verify ------------------------------------------------------
WORK="$(mktemp -d -t package-libsodium.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/download" "$WORK/src" "$WORK/install" "$WORK/pkg"

tarball="libsodium-$VERSION.tar.gz"
step "Downloading $tarball"
curl -fsSL -o "$WORK/download/$tarball" "$RELEASES_URL/$tarball"
curl -fsSL -o "$WORK/download/$tarball.minisig" "$RELEASES_URL/$tarball.minisig"

step "Verifying the release signature"
minisign -V -q -P "$LIBSODIUM_MINISIGN_KEY" -m "$WORK/download/$tarball" ||
    die "$tarball is NOT signed by the libsodium release key; refusing to package it"
echo "    signature OK"

# --- Build ----------------------------------------------------------------
tar -xzf "$WORK/download/$tarball" -C "$WORK/src"
srcdir="$(find "$WORK/src" -mindepth 1 -maxdepth 1 -type d | head -1)"
[ -x "$srcdir/configure" ] || die "unexpected tarball layout (no configure script)"

step "Building a static library ($JOBS jobs)"
log="$WORK/build.log"
run() {
    # Keep the (noisy) build output in a log; show the end of it if a step fails.
    if ! "$@" >> "$log" 2>&1; then
        echo "---- last lines of the build log ----" >&2
        tail -n 40 "$log" >&2
        die "'$*' failed"
    fi
}
cd "$srcdir"
# --with-pic so it links into position-independent executables (the default on most distros).
run env CC="$CC" ./configure --disable-shared --enable-static --with-pic --prefix="$WORK/install"
run make -j"$JOBS"
if [ "$CHECK" -eq 1 ]; then
    step "Running libsodium's test suite"
    run make -j"$JOBS" check
fi
run make install
cd "$PROJECT_DIR"

# --- Package --------------------------------------------------------------
step "Packaging sodium@$PKG_VERSION"
mkdir "$WORK/pkg/lib"
cp "$WORK/install/lib/libsodium.a" "$WORK/pkg/lib/"
cp -r "$WORK/install/include" "$WORK/pkg/include"
cp "$srcdir/LICENSE" "$WORK/pkg/license.txt"
cat > "$WORK/pkg/package.cm" <<EOF
name        = sodium
version     = $PKG_VERSION
platform    = $PLATFORM
description = Modern, portable, easy to use crypto library
libs        = libsodium.a
EOF

"$CHECKMAKE" pack "$KEY" "$WORK/pkg" "$REPO" | grep -v '^signed '

echo
echo "Done. Projects whose version range allows $PKG_VERSION pick it up with: checkmake update sodium"
