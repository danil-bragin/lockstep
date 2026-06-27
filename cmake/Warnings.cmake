# cmake/Warnings.cmake — the warnings-as-errors policy for first-party code.
#
# Provides an INTERFACE target `lockstep_warnings`. Link it PRIVATE into every
# first-party target so the flags do NOT leak to consumers. Third-party /
# vendored code (none yet) should NOT link this.
#
#   target_link_libraries(my_target PRIVATE lockstep_warnings)

add_library(lockstep_warnings INTERFACE)

# clang (incl. Apple clang) and gcc share this flag set.
set(_lockstep_warning_flags
  -Wall
  -Wextra
  -Werror
  -Wpedantic
  # Designated-init that intentionally omits trailing fields is idiomatic here (e.g.
  # Column{.name=.., .type=..}). Apple clang ignores this under -Wextra; upstream clang
  # (the CI toolchain) flags it -> a host/CI divergence. The omitted fields fall back to
  # their in-struct defaults by design, so silence this one check while keeping -Wextra.
  -Wno-missing-field-initializers
)

target_compile_options(lockstep_warnings INTERFACE ${_lockstep_warning_flags})
