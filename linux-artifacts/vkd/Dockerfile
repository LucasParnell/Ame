# Build tools only; sources are mounted read-only and artifacts go to /out.
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    clang lld binutils python3 libc6-dev mesa-vulkan-drivers libvulkan-dev \
    vulkan-validationlayers ca-certificates && rm -rf /var/lib/apt/lists/*
