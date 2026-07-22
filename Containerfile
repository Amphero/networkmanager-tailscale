# Build-Umgebung für das NetworkManager-Tailscale-Plugin.
# Arch-Basis, damit die Artefakte ABI-kompatibel zum Arch-Host sind.
FROM docker.io/library/archlinux:latest

RUN pacman -Syu --noconfirm --needed \
        gcc make pkgconf glib2 glib2-devel \
        libnm gtk4 json-glib curl qt6-base \
        plasma-nm kcoreaddons kwidgetsaddons networkmanager-qt \
        dbus python \
        base-devel networkmanager tailscale \
    && pacman -Scc --noconfirm \
    && useradd -m builder
