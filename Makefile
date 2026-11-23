# Host side: everything runs in rootless Podman, nothing gets installed on the host.
IMAGE  ?= nm-tailscale-plugin-build
PODMAN ?= podman
RUN     = $(PODMAN) run --rm -v "$(CURDIR)":/src -w /src $(IMAGE)

build:
	$(RUN) make -f build.mk all

container:
	$(PODMAN) build -t $(IMAGE) .

check:
	$(RUN) make -f build.mk check check-editor

shell:
	$(PODMAN) run --rm -it -v "$(CURDIR)":/src -w /src $(IMAGE) bash

PKGVER = $(shell sed -n 's/^pkgver=//p' packaging/PKGBUILD)

# Build the Arch packages from the committed state (HEAD) -> dist/*.pkg.tar.zst
pkg:
	git archive --format=tar.gz --prefix=networkmanager-tailscale-$(PKGVER)/ \
	    -o packaging/networkmanager-tailscale-$(PKGVER).tar.gz HEAD
	$(PODMAN) run --rm -v "$(CURDIR)":/src $(IMAGE) sh -c '\
	    cp -r /src/packaging /tmp/pkg && chown -R builder /tmp/pkg && \
	    cd /tmp/pkg && runuser -u builder -- makepkg -f --noconfirm --skipinteg && \
	    mkdir -p /src/dist && cp *.pkg.tar.zst /src/dist/'

DEB_IMAGES ?= debian:trixie ubuntu:26.04
RPM_IMAGE  ?= fedora:latest

# Build the Debian/Ubuntu and Fedora packages from the working tree -> dist/
deb:
	for img in $(DEB_IMAGES); do \
	    $(PODMAN) run --rm -v "$(CURDIR)":/src -w /src docker.io/library/$$img sh tools/build-deb.sh || exit 1; \
	done

rpm:
	$(PODMAN) run --rm -v "$(CURDIR)":/src -w /src docker.io/library/$(RPM_IMAGE) sh tools/build-rpm.sh

clean:
	rm -rf dist packaging/*.tar.gz

.PHONY: build container check shell clean pkg deb rpm
