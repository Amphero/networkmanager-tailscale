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

clean:
	rm -rf dist

.PHONY: build container check shell clean
