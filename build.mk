# Runs INSIDE the container or a package build:
# make -f build.mk [all|core|plasma|check|install|clean]
CC     ?= gcc
CXX    ?= g++
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
DIST   := dist

# Install layout. The defaults fit Arch and Debian/Ubuntu, Fedora needs
# NM_LIBEXECDIR=/usr/libexec.
PREFIX           ?= /usr
LIBDIR           ?= $(shell pkg-config --variable=libdir libnm)
NM_LIBEXECDIR    ?= $(PREFIX)/lib/NetworkManager
NM_PLUGINDIR     ?= $(LIBDIR)/NetworkManager
NM_VPNDIR        ?= $(PREFIX)/lib/NetworkManager/VPN
NM_DISPATCHERDIR ?= $(PREFIX)/lib/NetworkManager/dispatcher.d
DBUS_SYSTEMDIR   ?= $(PREFIX)/share/dbus-1/system.d
SYSTEMD_UNITDIR  ?= $(PREFIX)/lib/systemd/system
KF6_INCDIR       ?= $(PREFIX)/include/KF6
MOC              ?= $(shell pkg-config --variable=libexecdir Qt6Core)/moc
QT_PLUGINDIR     ?= $(shell $(shell pkg-config --variable=bindir Qt6Core)/qtpaths6 --query QT_INSTALL_PLUGINS)

PKG_SERVICE     := libnm libcurl json-glib-1.0
PKG_PLUGIN      := libnm gmodule-2.0
PKG_EDITOR      := libnm gtk+-3.0 gmodule-2.0 libcurl json-glib-1.0
PKG_EDITOR_GTK4 := libnm gtk4 gmodule-2.0 libcurl json-glib-1.0

LOCALAPI := shared/nm-tailscale-localapi.c

CORE := $(DIST)/nm-tailscale-service \
        $(DIST)/libnm-vpn-plugin-tailscale.so \
        $(DIST)/libnm-vpn-plugin-tailscale-editor.so \
        $(DIST)/libnm-gtk4-vpn-plugin-tailscale-editor.so \
        $(DIST)/nm-tailscale-service.name \
        $(DIST)/nm-tailscale-service.conf \
        $(DIST)/nm-tailscale-dispatcher \
        $(DIST)/nm-tailscale-shutdown \
        $(DIST)/tailscaled-nm-tailscale.conf
PLASMA := $(DIST)/plasmanetworkmanagement_tailscaleui.so \
          $(DIST)/plasmanetworkmanagement_tailscaleui-stub.so

all: core plasma
core: $(CORE)
plasma: $(PLASMA)

$(DIST):
	mkdir -p $@

$(DIST)/nm-tailscale-service: src/nm-tailscale-service.c $(LOCALAPI) shared/nm-tailscale.h shared/nm-tailscale-localapi.h | $(DIST)
	$(CC) $(CFLAGS) -Ishared -o $@ src/nm-tailscale-service.c $(LOCALAPI) \
	    $$(pkg-config --cflags --libs $(PKG_SERVICE))

$(DIST)/libnm-vpn-plugin-tailscale.so: properties/nm-tailscale-editor-plugin.c shared/nm-tailscale.h properties/libnm-vpn-plugin-tailscale.ver | $(DIST)
	$(CC) $(CFLAGS) -Ishared -shared -fPIC -o $@ $< \
	    -Wl,--version-script=properties/libnm-vpn-plugin-tailscale.ver \
	    $$(pkg-config --cflags --libs $(PKG_PLUGIN)) -ldl

# Editor module for GTK3 hosts (nm-connection-editor)
$(DIST)/libnm-vpn-plugin-tailscale-editor.so: properties/nm-tailscale-editor.c $(LOCALAPI) shared/nm-tailscale.h shared/nm-tailscale-localapi.h properties/nm-tailscale-editor.ver | $(DIST)
	$(CC) $(CFLAGS) -Ishared -shared -fPIC -o $@ properties/nm-tailscale-editor.c $(LOCALAPI) \
	    -Wl,--version-script=properties/nm-tailscale-editor.ver \
	    $$(pkg-config --cflags --libs $(PKG_EDITOR))

# The same editor source as above, built for GTK4 hosts (GNOME Settings)
$(DIST)/libnm-gtk4-vpn-plugin-tailscale-editor.so: properties/nm-tailscale-editor.c $(LOCALAPI) shared/nm-tailscale.h shared/nm-tailscale-localapi.h properties/nm-tailscale-editor.ver | $(DIST)
	$(CC) $(CFLAGS) -Ishared -shared -fPIC -o $@ properties/nm-tailscale-editor.c $(LOCALAPI) \
	    -Wl,--version-script=properties/nm-tailscale-editor.ver \
	    $$(pkg-config --cflags --libs $(PKG_EDITOR_GTK4))

# Native plasma-nm plugin: built against the private headers of the exact
# installed plasma-nm version (reference/plasma-nm, the tag has to match the
# host version) and linked against the installed libplasmanm_editor.so.
PLASMA_INC  = -Ireference/plasma-nm/libs/editor -Ireference/plasma-nm/libs/editor/widgets -Iplasma \
              -I$(KF6_INCDIR)/KCoreAddons -I$(KF6_INCDIR)/KWidgetsAddons -I$(KF6_INCDIR)/NetworkManagerQt
PLASMA_PKGS = Qt6Widgets Qt6DBus Qt6Concurrent libnm libcurl

$(DIST)/plasmanetworkmanagement_tailscaleui.so: plasma/tailscaleui.cpp $(LOCALAPI) plasma/plasmanetworkmanagement_tailscaleui.json plasma/plasmanm_editor_export.h shared/nm-tailscale.h shared/nm-tailscale-localapi.h | $(DIST)
	$(MOC) $$(pkg-config --cflags-only-I $(PLASMA_PKGS)) $(PLASMA_INC) -Ishared \
	    plasma/tailscaleui.cpp -o $(DIST)/tailscaleui.moc
	$(CC) $(CFLAGS) -fPIC -Ishared -c $(LOCALAPI) -o $(DIST)/localapi.o \
	    $$(pkg-config --cflags libnm libcurl)
	$(CXX) -O2 -Wall -std=c++17 -fPIC -shared -Ishared $(PLASMA_INC) -I$(DIST) -o $@ \
	    plasma/tailscaleui.cpp $(DIST)/localapi.o \
	    $$(pkg-config --cflags --libs $(PLASMA_PKGS)) \
	    -lKF6CoreAddons -lKF6WidgetsAddons -lKF6NetworkManagerQt \
	    $(LIBDIR)/libplasmanm_editor.so $(LIBDIR)/libplasmanm_internal.so
	rm -f $(DIST)/tailscaleui.moc $(DIST)/localapi.o

# Metadata stub as a fallback in case the native plugin stops loading
# after a Plasma update (same target path and name).
$(DIST)/plasmanetworkmanagement_tailscaleui-stub.so: plasma/stub.cpp plasma/plasmanetworkmanagement_tailscaleui.json | $(DIST)
	$(MOC) plasma/stub.cpp -o $(DIST)/stub.moc
	$(CXX) $(CFLAGS) -std=c++17 -fPIC -shared -Iplasma -I$(DIST) -o $@ plasma/stub.cpp \
	    $$(pkg-config --cflags --libs Qt6Core)
	rm -f $(DIST)/stub.moc

# files that carry install paths
SUBST = sed -e 's|@NM_LIBEXECDIR@|$(NM_LIBEXECDIR)|g' -e 's|@NM_PLUGINDIR@|$(NM_PLUGINDIR)|g'

$(DIST)/nm-tailscale-service.name: nm-tailscale-service.name.in | $(DIST)
	$(SUBST) $< > $@

$(DIST)/nm-tailscale-service.conf: nm-tailscale-service.conf | $(DIST)
	cp $< $@

$(DIST)/nm-tailscale-dispatcher: nm-tailscale-dispatcher | $(DIST)
	cp $< $@

$(DIST)/nm-tailscale-shutdown: nm-tailscale-shutdown.in | $(DIST)
	$(SUBST) $< > $@
	chmod 755 $@

$(DIST)/tailscaled-nm-tailscale.conf: tailscaled-nm-tailscale.conf.in | $(DIST)
	$(SUBST) $< > $@

check: core
	sh tests/smoke-test.sh

install: install-core install-plasma

install-core: core
	install -Dm755 $(DIST)/nm-tailscale-service $(DESTDIR)$(NM_LIBEXECDIR)/nm-tailscale-service
	install -Dm755 $(DIST)/nm-tailscale-shutdown $(DESTDIR)$(NM_LIBEXECDIR)/nm-tailscale-shutdown
	install -Dm755 $(DIST)/libnm-vpn-plugin-tailscale.so $(DESTDIR)$(NM_PLUGINDIR)/libnm-vpn-plugin-tailscale.so
	install -Dm755 $(DIST)/libnm-vpn-plugin-tailscale-editor.so $(DESTDIR)$(NM_PLUGINDIR)/libnm-vpn-plugin-tailscale-editor.so
	install -Dm755 $(DIST)/libnm-gtk4-vpn-plugin-tailscale-editor.so $(DESTDIR)$(NM_PLUGINDIR)/libnm-gtk4-vpn-plugin-tailscale-editor.so
	install -Dm644 $(DIST)/nm-tailscale-service.name $(DESTDIR)$(NM_VPNDIR)/nm-tailscale-service.name
	install -Dm644 $(DIST)/nm-tailscale-service.conf $(DESTDIR)$(DBUS_SYSTEMDIR)/nm-tailscale-service.conf
	install -Dm755 $(DIST)/nm-tailscale-dispatcher $(DESTDIR)$(NM_DISPATCHERDIR)/90-nm-tailscale
	install -Dm644 $(DIST)/tailscaled-nm-tailscale.conf $(DESTDIR)$(SYSTEMD_UNITDIR)/tailscaled.service.d/nm-tailscale.conf

install-plasma: $(DIST)/plasmanetworkmanagement_tailscaleui.so
	install -Dm755 $< $(DESTDIR)$(QT_PLUGINDIR)/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so

clean:
	rm -rf $(DIST)

.PHONY: all core plasma check install install-core install-plasma clean
