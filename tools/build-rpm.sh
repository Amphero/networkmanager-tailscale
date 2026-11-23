#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# builds the .rpm packages into dist/, run it in a Fedora container from
# the source tree:
#   podman run --rm -v "$PWD":/src -w /src fedora:latest sh tools/build-rpm.sh
set -eu

src=$PWD
spec=packaging/rpm/NetworkManager-tailscale.spec
ver=$(sed -n 's/^Version: *//p' "$spec")

dnf -y -q install rpm-build 'dnf-command(builddep)' > /dev/null
dnf -y -q builddep "$spec" > /dev/null
plasmanm=$(rpm -q --qf '%{VERSION}' plasma-nm)

top=$(mktemp -d)
mkdir -p "$top/SOURCES" "$top/src/networkmanager-tailscale-$ver"
tar -C . --exclude=./dist --exclude=./reference --exclude=./.git -cf - . |
	tar -C "$top/src/networkmanager-tailscale-$ver" -xf -
tar -C "$top/src" -czf "$top/SOURCES/networkmanager-tailscale-$ver.tar.gz" "networkmanager-tailscale-$ver"
curl -sSfL -o "$top/SOURCES/plasma-nm-v$plasmanm.tar.gz" \
	"https://invent.kde.org/plasma/plasma-nm/-/archive/v$plasmanm/plasma-nm-v$plasmanm.tar.gz"

rpmbuild -bb --define "_topdir $top" --define "plasmanm_version $plasmanm" "$spec"
mkdir -p "$src/dist"
find "$top/RPMS" -name '*.rpm' ! -name '*-debug*' -exec cp {} "$src/dist/" \;
