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

# --- TLS TRANSPORT ENCRYPTION (OpenSSL) [LINUX ONLY] --------------------------
# The prod transport (ProdNetwork over real TCP) is wrapped in real TLS (ProdTls.hpp).
# OpenSSL is the crypto provider; the crypto calls live ENTIRELY in providers/prod (the
# lint-exempt boundary). When OpenSSL is present on Linux we define LOCKSTEP_TLS (which
# turns on the ProdTls header + the TLS branches in ProdNetwork) and link libssl/libcrypto
# INTERFACE so every consumer (lockstepd / lockstep_admin / the TLS test) gets the headers,
# the define, and the link. On macOS (no prod transport built) or without OpenSSL the
# define is absent and the transport stays plaintext — the Mac host build stays green.
if(UNIX AND NOT APPLE)
  find_package(OpenSSL)
  if(OpenSSL_FOUND)
    target_compile_definitions(lockstep_providers_prod INTERFACE LOCKSTEP_TLS)
    target_link_libraries(lockstep_providers_prod INTERFACE
      OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "lockstep: TLS transport ENABLED (OpenSSL ${OPENSSL_VERSION})")
  else()
    message(STATUS "lockstep: OpenSSL not found — prod transport stays PLAINTEXT")
  endif()
endif()
