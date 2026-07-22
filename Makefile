# Host-Seite: alles läuft in rootless Podman, nichts wird auf dem Host installiert.
IMAGE  ?= nm-tailscale-plugin-build
PODMAN ?= podman
RUN     = $(PODMAN) run --rm -v "$(CURDIR)":/src -w /src $(IMAGE)

build:
	$(RUN) make -f build.mk all

container:
	$(PODMAN) build -t $(IMAGE) .

check:
	$(RUN) make -f build.mk check

shell:
	$(PODMAN) run --rm -it -v "$(CURDIR)":/src -w /src $(IMAGE) bash

PKGVER = $(shell sed -n 's/^pkgver=//p' packaging/PKGBUILD)

# Arch-Pakete aus dem committeten Stand (HEAD) bauen -> dist/*.pkg.tar.zst
pkg:
	git archive --format=tar.gz --prefix=networkmanager-tailscale-$(PKGVER)/ \
	    -o packaging/networkmanager-tailscale-$(PKGVER).tar.gz HEAD
	$(PODMAN) run --rm -v "$(CURDIR)":/src $(IMAGE) sh -c '\
	    cp -r /src/packaging /tmp/pkg && chown -R builder /tmp/pkg && \
	    cd /tmp/pkg && runuser -u builder -- makepkg -f --noconfirm --skipinteg && \
	    mkdir -p /src/dist && cp *.pkg.tar.zst /src/dist/'

clean:
	rm -rf dist packaging/*.tar.gz

.PHONY: build container check shell clean pkg
