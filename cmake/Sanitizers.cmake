# cmake/Sanitizers.cmake — per-sanitizer flag sets.
#
# Driven by the cache variable LOCKSTEP_SANITIZER (set by the CMake presets:
# asan / tsan / ubsan / msan). When empty (debug/release) no sanitizer is added.
#
# Provides an INTERFACE target `lockstep_sanitizers`; link it PRIVATE into
# first-party targets. The flags must appear at BOTH compile and link time,
# which an INTERFACE target propagates automatically.
#
# HOST CAVEAT: MSan (MemorySanitizer) is Linux/LLVM-only. Apple clang +
# macOS libc++ do NOT ship an MSan runtime, and MSan additionally needs an
# instrumented libc++ to avoid false positives. The `msan` preset therefore
# only builds for real in CI (ubuntu + upstream llvm). On this host configuring
# the msan preset emits a loud warning and falls back to an un-instrumented
# build so the tree still configures; `scripts/gate.sh` prints
# `[SKIP host-limited]` for it.

set(LOCKSTEP_SANITIZER "" CACHE STRING "Sanitizer to enable: asan|tsan|ubsan|msan (empty = none)")
set_property(CACHE LOCKSTEP_SANITIZER PROPERTY STRINGS "" asan tsan ubsan msan)

add_library(lockstep_sanitizers INTERFACE)

# _san_flags : flags for BOTH compile and link (the sanitizer + the libc++
#              header include path).
# _san_link_flags : link-ONLY flags (-stdlib, lib search path, rpath, -lc++/
#              -lc++abi); passing these at compile time trips
#              -Werror=unused-command-line-argument.
set(_san_flags "")
set(_san_link_flags "")

if(LOCKSTEP_SANITIZER STREQUAL "asan")
  # AddressSanitizer + LeakSanitizer. -fno-omit-frame-pointer for usable traces.
  list(APPEND _san_flags -fsanitize=address -fno-omit-frame-pointer -g)

elseif(LOCKSTEP_SANITIZER STREQUAL "tsan")
  # ThreadSanitizer. Lockstep core is single-threaded by design, but TSan still
  # guards provider/runtime code and is part of the universal gate.
  list(APPEND _san_flags -fsanitize=thread -fno-omit-frame-pointer -g)

elseif(LOCKSTEP_SANITIZER STREQUAL "ubsan")
  # UndefinedBehaviorSanitizer. Trap-on-error so any UB fails the test.
  list(APPEND _san_flags
    -fsanitize=undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
    -g)

elseif(LOCKSTEP_SANITIZER STREQUAL "msan")
  if(APPLE)
    message(WARNING
      "LOCKSTEP_SANITIZER=msan is unsupported on macOS/Apple-libc++ "
      "(no MSan runtime / no instrumented libc++). Configuring WITHOUT MSan; "
      "the real MSan job runs in CI on ubuntu+llvm. See cmake/Sanitizers.cmake.")
  else()
    list(APPEND _san_flags
      -fsanitize=memory
      -fsanitize-memory-track-origins=2
      -fno-omit-frame-pointer
      -g)
    # REAL MSan needs the standard library itself built with -fsanitize=memory,
    # else MSan false-positives in stdlib internals (std::string/optional/map).
    # tools/docker builds an instrumented libc++ at $LOCKSTEP_MSAN_LIBCXX; when
    # present, replace the default stdlib with it. Without it, MSan still runs but
    # WILL emit stdlib false positives (loudly warned).
    if(DEFINED ENV{LOCKSTEP_MSAN_LIBCXX} AND NOT "$ENV{LOCKSTEP_MSAN_LIBCXX}" STREQUAL "")
      set(_msan_cxx "$ENV{LOCKSTEP_MSAN_LIBCXX}")
      message(STATUS "MSan: using instrumented libc++ at ${_msan_cxx}")
      # Compile-side: drop the default C++ include dirs and point at the
      # instrumented headers. (-stdlib selects the runtime and is a no-op at
      # compile under -nostdinc++, so it lives in the link-only list below;
      # passing it at compile time trips -Werror=unused-command-line-argument.)
      list(APPEND _san_flags
        -nostdinc++
        "-isystem${_msan_cxx}/include/c++/v1")
      # Link-ONLY: select libc++, the library search path, rpath, and the libs
      # themselves. Passed at compile time these are unused command-line
      # arguments and -Werror turns that into a build error.
      list(APPEND _san_link_flags
        -stdlib=libc++
        "-L${_msan_cxx}/lib"
        "-Wl,-rpath,${_msan_cxx}/lib"
        -lc++ -lc++abi)
    else()
      message(WARNING
        "MSan WITHOUT an instrumented libc++ (LOCKSTEP_MSAN_LIBCXX unset): "
        "EXPECT FALSE POSITIVES in stdlib internals. Build the instrumented "
        "libc++ (tools/docker) for a real MSan run.")
    endif()
  endif()

elseif(NOT LOCKSTEP_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown LOCKSTEP_SANITIZER='${LOCKSTEP_SANITIZER}' (want: asan tsan ubsan msan)")
endif()

if(_san_flags)
  target_compile_options(lockstep_sanitizers INTERFACE ${_san_flags})
  target_link_options(lockstep_sanitizers INTERFACE ${_san_flags})
endif()
if(_san_link_flags)
  target_link_options(lockstep_sanitizers INTERFACE ${_san_link_flags})
endif()
