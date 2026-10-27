# PS5 RADV release archive, built on an x86-64 Debian trixie container.
#
# The image mirrors PS5LM's CI (.github/workflows/native-build.yml, "PS5 title"
# job): LLVM 19 + SPIRV/glslang/Mesa host tools on debian:trixie-slim, 2 build
# jobs. It is linux/amd64 because PS5 Vulkan's supported host is x86-64 Linux
# (PS5LM docs/BUILDING.md); on Apple silicon Docker runs it under Rosetta.
# SPDX-License-Identifier: GPL-3.0-or-later
FROM --platform=linux/amd64 debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive \
    BUILD_JOBS=2 \
    LLVM_CONFIG=/usr/bin/llvm-config-19

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential git ca-certificates wget curl unzip rsync tar \
        cmake ninja-build meson pkgconf flex bison xxd \
        clang-19 llvm-19-dev lld-19 libclang-rt-19-dev libclang-19-dev \
        libclang-cpp19-dev libllvmspirvlib-19-dev libclc-19-dev \
        spirv-tools spirv-headers glslang-tools glslc \
        python3 python3-mako python3-yaml python3-packaging python3-setuptools \
        zlib1g-dev libzstd-dev libffi-dev libedit-dev libxml2-dev libvulkan-dev \
    && rm -rf /var/lib/apt/lists/*

# The SDK's prospero-* wrappers call the host "clang" by name and find the rest
# through llvm-config; Debian keeps versioned binaries.
RUN set -eux; \
    for tool in clang clang++ clang-cpp ld.lld llvm-ar llvm-nm llvm-objcopy llvm-ranlib llvm-strip; do \
        bare=${tool}; \
        [ -e /usr/lib/llvm-19/bin/$bare ] || ln -sf /usr/bin/${tool}-19 /usr/lib/llvm-19/bin/$bare; \
        ln -sf /usr/lib/llvm-19/bin/$bare /usr/local/bin/$bare; \
    done

COPY build-in-container.sh /usr/local/bin/build-in-container
RUN chmod +x /usr/local/bin/build-in-container

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/build-in-container"]
