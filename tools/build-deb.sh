#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# builds the .deb packages into dist/, run it in a Debian or Ubuntu
# container from the source tree:
#   podman run --rm -v "$PWD":/src -w /src debian:trixie sh tools/build-deb.sh
set -eu

src=$PWD
. /etc/os-release
case $ID in
debian) suffix="~deb$VERSION_ID" ;;
*) suffix="~$ID$VERSION_ID" ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends dpkg-dev > /dev/null

build=$(mktemp -d)/network-manager-tailscale
mkdir -p "$build"
tar -C . --exclude=./dist --exclude=./reference --exclude=./.git -cf - . | tar -C "$build" -xf -
cp -r packaging/debian "$build/debian"
sed -i "1s/(\([^)]*\))/(\1$suffix)/" "$build/debian/changelog"

cd "$build"
apt-get build-dep -y -qq ./ > /dev/null
dpkg-buildpackage -b -us -uc
mkdir -p "$src/dist"
find .. -maxdepth 1 -name '*.deb' ! -name '*-dbgsym_*' -exec cp {} "$src/dist/" \;
