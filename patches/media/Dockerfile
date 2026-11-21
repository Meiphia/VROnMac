FROM --platform=linux/amd64 debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential bison flex clang lld mingw-w64 pkg-config \
    libfreetype-dev libgnutls28-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
