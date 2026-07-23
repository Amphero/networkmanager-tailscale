# Läuft IM Container: make -f build.mk [all|check|clean]
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
DIST   := dist

PKG_SERVICE := libnm libcurl json-glib-1.0
PKG_PLUGIN  := libnm gmodule-2.0
PKG_EDITOR  := libnm gtk4 gmodule-2.0 libcurl json-glib-1.0

LOCALAPI := shared/nm-tailscale-localapi.c

all: $(DIST)/nm-tailscale-service \
     $(DIST)/libnm-vpn-plugin-tailscale.so \
     $(DIST)/libnm-gtk4-vpn-plugin-tailscale-editor.so \
     $(DIST)/plasmanetworkmanagement_tailscaleui.so \
     $(DIST)/plasmanetworkmanagement_tailscaleui-stub.so \
     $(DIST)/nm-tailscale-service.name \
     $(DIST)/nm-tailscale-service.conf

$(DIST):
	mkdir -p $@

$(DIST)/nm-tailscale-service: src/nm-tailscale-service.c $(LOCALAPI) shared/nm-tailscale.h shared/nm-tailscale-localapi.h | $(DIST)
	$(CC) $(CFLAGS) -Ishared -o $@ src/nm-tailscale-service.c $(LOCALAPI) \
	    $$(pkg-config --cflags --libs $(PKG_SERVICE))

$(DIST)/libnm-vpn-plugin-tailscale.so: properties/nm-tailscale-editor-plugin.c shared/nm-tailscale.h properties/libnm-vpn-plugin-tailscale.ver | $(DIST)
	$(CC) $(CFLAGS) -Ishared -shared -fPIC -o $@ $< \
	    -Wl,--version-script=properties/libnm-vpn-plugin-tailscale.ver \
	    $$(pkg-config --cflags --libs $(PKG_PLUGIN)) -ldl

$(DIST)/libnm-gtk4-vpn-plugin-tailscale-editor.so: properties/nm-tailscale-editor.c $(LOCALAPI) shared/nm-tailscale.h shared/nm-tailscale-localapi.h properties/libnm-gtk4-vpn-plugin-tailscale-editor.ver | $(DIST)
	$(CC) $(CFLAGS) -Ishared -shared -fPIC -o $@ properties/nm-tailscale-editor.c $(LOCALAPI) \
	    -Wl,--version-script=properties/libnm-gtk4-vpn-plugin-tailscale-editor.ver \
	    $$(pkg-config --cflags --libs $(PKG_EDITOR))

# Natives plasma-nm-Plugin: baut gegen die privaten Header der exakt
# installierten plasma-nm-Version (reference/plasma-nm, Tag muss zur
# Host-Version passen) und linkt gegen das installierte libplasmanm_editor.so.
PLASMA_INC  = -Ireference/plasma-nm/libs/editor -Ireference/plasma-nm/libs/editor/widgets -Iplasma \
              -I/usr/include/KF6/KCoreAddons -I/usr/include/KF6/KWidgetsAddons -I/usr/include/KF6/NetworkManagerQt
PLASMA_PKGS = Qt6Widgets Qt6DBus Qt6Concurrent libnm libcurl

$(DIST)/plasmanetworkmanagement_tailscaleui.so: plasma/tailscaleui.cpp $(LOCALAPI) plasma/plasmanetworkmanagement_tailscaleui.json plasma/plasmanm_editor_export.h shared/nm-tailscale.h shared/nm-tailscale-localapi.h | $(DIST)
	/usr/lib/qt6/moc $$(pkg-config --cflags-only-I $(PLASMA_PKGS)) $(PLASMA_INC) -Ishared \
	    plasma/tailscaleui.cpp -o $(DIST)/tailscaleui.moc
	$(CC) $(CFLAGS) -fPIC -Ishared -c $(LOCALAPI) -o $(DIST)/localapi.o \
	    $$(pkg-config --cflags libnm libcurl)
	g++ -O2 -Wall -std=c++17 -fPIC -shared -Ishared $(PLASMA_INC) -I$(DIST) -o $@ \
	    plasma/tailscaleui.cpp $(DIST)/localapi.o \
	    $$(pkg-config --cflags --libs $(PLASMA_PKGS)) \
	    -lKF6CoreAddons -lKF6WidgetsAddons -lKF6NetworkManagerQt \
	    /usr/lib/libplasmanm_editor.so /usr/lib/libplasmanm_internal.so
	rm -f $(DIST)/tailscaleui.moc $(DIST)/localapi.o

# Metadaten-Stub als Fallback, falls das native Plugin nach einem
# Plasma-Update nicht mehr laedt (gleicher Zielpfad/-name).
$(DIST)/plasmanetworkmanagement_tailscaleui-stub.so: plasma/stub.cpp plasma/plasmanetworkmanagement_tailscaleui.json | $(DIST)
	/usr/lib/qt6/moc plasma/stub.cpp -o $(DIST)/stub.moc
	g++ $(CFLAGS) -std=c++17 -fPIC -shared -Iplasma -I$(DIST) -o $@ plasma/stub.cpp \
	    $$(pkg-config --cflags --libs Qt6Core)
	rm -f $(DIST)/stub.moc

$(DIST)/nm-tailscale-service.name: nm-tailscale-service.name | $(DIST)
	cp $< $@

$(DIST)/nm-tailscale-service.conf: nm-tailscale-service.conf | $(DIST)
	cp $< $@

check: all
	sh tests/smoke-test.sh

clean:
	rm -rf $(DIST)

.PHONY: all check clean
