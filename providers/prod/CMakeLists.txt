# providers/prod — production providers (real chrono/entropy/sockets, reactor).
# Phase 7 S2 lands the FIRST prod providers (ProdRandom + ProdClock) plus the
# record-replay TRACE scaffolding (ReplayTrace), all header-only behind the frozen
# core interfaces. NOTE: providers/ is the lint-exempt boundary dir (cardinal rule
# 1) — real std::chrono / std::random_device are allowed HERE and ONLY here.
#
# Header-only -> INTERFACE library. Consumers link `lockstep_providers_prod` and
# get <lockstep/prod/...> on their include path. Depends on lockstep_core for the
# frozen interfaces (IClock/IRandom/Error/Future).
add_library(lockstep_providers_prod INTERFACE)

target_include_directories(lockstep_providers_prod INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(lockstep_providers_prod INTERFACE lockstep_core)
target_compile_features(lockstep_providers_prod INTERFACE cxx_std_23)
