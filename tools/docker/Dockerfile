# Lockstep Linux dev/test image — the FULL §8 gate runs here (MSan, clang-tidy,
# scan-build all work on Linux, unlike macOS/Apple-clang). arm64-native on Apple
# Silicon (no emulation). This is also closer to prod (the system targets Linux).
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang \
      clang-tidy \
      clang-tools \
      llvm \
      cmake \
      ninja-build \
      make \
      python3 \
      git \
      default-jre-headless \
      libc++-dev libc++abi-dev \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Default compiler = clang (the project pins C++23; gate.sh drives presets).
ENV CC=clang CXX=clang++
WORKDIR /work
