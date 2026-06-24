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

# Sanitizer runtimes (libclang_rt.asan/tsan/ubsan/msan) — a SEPARATE layer so the
# big base apt layer above stays cached on rebuild. Without these the ASan/TSan/
# UBSan/MSan link step fails (ld: cannot find libclang_rt.*-aarch64.a).
RUN apt-get update && apt-get install -y --no-install-recommends \
      libclang-rt-18-dev \
 && rm -rf /var/lib/apt/lists/*

# MSan-instrumented libc++/libc++abi/libunwind — REAL MSan needs the standard
# library itself built with -fsanitize=memory, else MSan false-positives in
# stdlib internals (std::string/optional/map). cmake/Sanitizers.cmake picks this
# up via $LOCKSTEP_MSAN_LIBCXX. Heavy one-time build (~20-40 min), cached as its
# own layer. The image's clang is 18.1.x; the 18.1.8 runtimes are compatible.
# (git + ninja-build + cmake come from the base apt layer above — no apt here.)
RUN git clone --depth 1 --branch llvmorg-18.1.8 \
      https://github.com/llvm/llvm-project /tmp/llvm \
 && cmake -G Ninja -S /tmp/llvm/runtimes -B /tmp/llvm-msan \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
      -DLLVM_USE_SANITIZER=MemoryWithOrigins \
      -DCMAKE_INSTALL_PREFIX=/opt/libcxx-msan \
 && ninja -C /tmp/llvm-msan cxx cxxabi unwind \
 && ninja -C /tmp/llvm-msan install-cxx install-cxxabi install-unwind \
 && rm -rf /tmp/llvm /tmp/llvm-msan
ENV LOCKSTEP_MSAN_LIBCXX=/opt/libcxx-msan

# TLS TRANSPORT ENCRYPTION — OpenSSL development headers + pkg-config for the prod TLS
# transport (providers/prod/ProdTls.hpp wraps the real TCP transport in TLS/mTLS). The
# runtime libssl3 is already in the ubuntu:24.04 base; libssl-dev adds /usr/include/openssl
# + the link archives so CMake's find_package(OpenSSL) resolves and the TLS targets build
# (and the prod_tls_test generates dev certs with the `openssl` CLI this package provides).
# Its own SEPARATE layer so the heavy cached base + MSan-libc++ layers above are NOT
# rebuilt. ADDED for the TLS-transport work.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl-dev \
      pkg-config \
 && rm -rf /var/lib/apt/lists/*

# Default compiler = clang (the project pins C++23; gate.sh drives presets).
ENV CC=clang CXX=clang++
WORKDIR /work
