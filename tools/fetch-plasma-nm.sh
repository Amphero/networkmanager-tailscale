#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# fetch the plasma-nm sources matching the installed plasma-nm into
# reference/plasma-nm, the plasma plugin needs its private headers
set -eu

if command -v pacman > /dev/null; then
	ver=$(pacman -Q plasma-nm | cut -d' ' -f2)
elif command -v dpkg-query > /dev/null; then
	ver=$(dpkg-query -W -f='${Version}' plasma-nm)
else
	ver=$(rpm -q --qf '%{VERSION}' plasma-nm)
fi
[ -n "$ver" ] || { echo "plasma-nm is not installed" >&2; exit 1; }
ver=${ver#*:}
ver=${ver%%-*}

dest=${1:-reference/plasma-nm}
rm -rf "$dest"
mkdir -p "$dest"
curl -sSfL "https://invent.kde.org/plasma/plasma-nm/-/archive/v$ver/plasma-nm-v$ver.tar.gz" |
	tar -xz -C "$dest" --strip-components=1
echo "plasma-nm $ver in $dest"
