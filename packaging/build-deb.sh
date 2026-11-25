#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the Debian package of sp211-printer-app for the machine it runs on.
#
#   packaging/build-deb.sh VERSION
#
# Needs: build-essential libpappl-dev libusb-1.0-0-dev libmosquitto-dev python3
# dpkg-dev.
# Writes build/sp211-printer-app_VERSION_ARCH.deb. The package's dependencies
# are computed by dpkg-shlibdeps from the libraries the binary uses.

set -eu

VERSION=${1:?usage: packaging/build-deb.sh VERSION}
NAME=sp211-printer-app
ARCH=$(dpkg --print-architecture)
TOP=$(cd "$(dirname "$0")/.." && pwd)
STAGE=$TOP/build/deb/$NAME
OUT=$TOP/build/${NAME}_${VERSION}_${ARCH}.deb

cd "$TOP"
make clean
make build/sp211-printer-app MQTT=yes
make test
rm -rf "$STAGE"
make install DESTDIR="$STAGE" PREFIX=/usr SYSTEMD_UNITDIR=/usr/lib/systemd/system MQTT=yes

# Home Assistant blueprint, for the plug
install -d "$STAGE/usr/share/$NAME"
install -m 644 homeassistant/blueprints/sp211_power.yaml "$STAGE/usr/share/$NAME/"

# Documentation and licenses
DOC=$STAGE/usr/share/doc/$NAME
install -d "$DOC"
install -m 644 README.md NOTICE.md docs/MQTT.md "$DOC/"
{
  printf 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n'
  printf 'Upstream-Name: %s\nSource: https://github.com/Pulpyyyy/ricoh-sp211-printer-app\n\n' "$NAME"
  printf 'Files: *\nLicense: GPL-2.0-or-later\n'
  printf ' The source files are under GPL-2.0-or-later; the sp211-printer-app binary,\n'
  printf ' linked with PAPPL (Apache-2.0), is distributed under GPL-3.0-or-later.\n'
  printf ' See NOTICE.md for the origin of each part, and /usr/share/common-licenses/GPL-2\n'
  printf ' and /usr/share/common-licenses/GPL-3 for the license texts.\n'
} > "$DOC/copyright"
chmod 644 "$DOC/copyright"

# Dependencies of the binary, as dpkg-shlibdeps finds them
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$NAME" "$NAME" > "$TMP/debian/control"
DEPENDS=$(cd "$TMP" && dpkg-shlibdeps -O "$STAGE/usr/bin/$NAME" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
[ -n "$DEPENDS" ] || { echo "dpkg-shlibdeps found no dependencies" >&2; exit 1; }

install -d "$STAGE/DEBIAN"
SIZE=$(du -sk "$STAGE" --exclude=DEBIAN | cut -f1)
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $NAME
Version: $VERSION
Architecture: $ARCH
Maintainer: Pulpyyyy <Pulpyyyy@users.noreply.github.com>
Installed-Size: $SIZE
Depends: $DEPENDS
Recommends: avahi-daemon
Section: misc
Priority: optional
Homepage: https://github.com/Pulpyyyy/ricoh-sp211-printer-app
Description: IPP Everywhere printer for the Ricoh SP 211
 A Printer Application built on PAPPL: Windows, macOS, iOS, Android and
 Home Assistant print to a Ricoh SP 211 on USB without any driver, and read
 its toner level and page counter over IPP. Optionally it publishes the
 printer to Home Assistant over MQTT, with a power switch that a smart plug
 can follow. It listens on the IPP port, 631, so it must not run next to
 CUPS driving the same printer.
EOF
echo /etc/default/$NAME > "$STAGE/DEBIAN/conffiles"

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && [ -d /run/systemd/system ]; then
  systemctl daemon-reload
  systemctl enable sp211-printer-app.service
  systemctl restart sp211-printer-app.service || true
fi
EOF
cat > "$STAGE/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] && [ -d /run/systemd/system ]; then
  systemctl disable --now sp211-printer-app.service || true
fi
EOF
cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ]; then
  systemctl daemon-reload || true
fi
if [ "$1" = purge ]; then
  rm -rf /var/lib/sp211-printer-app
fi
EOF
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm" "$STAGE/DEBIAN/postrm"

dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
echo "$OUT"
