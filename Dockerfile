FROM debian:bookworm

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        gcc \
        binutils \
        make \
        grub-pc-bin \
        grub-common \
        xorriso \
        mtools && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /myos