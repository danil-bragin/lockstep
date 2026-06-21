# core/ — abstraction-boundary interface headers (header-only).
#
# `lockstep_core` is an INTERFACE (header-only) library: it has no compiled
# sources, only the pure-virtual boundary headers under include/. Everything
# above core/ codes against these interfaces; providers (sim/prod) implement
# them. core/ depends on nothing above it (spec C0.4).

add_library(lockstep_core INTERFACE)

target_include_directories(lockstep_core INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# These headers are C++23 and use std::span / nested namespaces.
target_compile_features(lockstep_core INTERFACE cxx_std_23)
