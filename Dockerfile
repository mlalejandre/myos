FROM debian:bookworm

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        gcc \
        binutils \
        make \
        grub-pc-bin \
        grub-common \
        xorriso \
        mtools \
        gcc-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /soma
