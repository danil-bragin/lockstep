# tests — Phase 0 smoke test + CTest registration.
#
# Frozen names (integration contract): target `lockstep_smoke_test`, registered
# with CTest as `add_test(NAME smoke ...)`. The test links the header-only
# INTERFACE lib `lockstep_core` (authored by agent A2) so integration across the
# boundary is exercised.

add_executable(lockstep_smoke_test smoke_test.cpp)

target_link_libraries(lockstep_smoke_test
  PRIVATE
    lockstep_warnings
    lockstep_sanitizers)

# Link lockstep_core when A2's target exists. Guarded so A1's tree still builds
# green before A2 lands; the smoke_test.cpp side also #if-guards the include.
# Once core/CMakeLists.txt is present, this is a hard link and the include path
# carries <lockstep/core/...> into the translation unit.
if(TARGET lockstep_core)
  target_link_libraries(lockstep_smoke_test PRIVATE lockstep_core)
else()
  message(WARNING
    "lockstep_core target not found (owned by agent A2). lockstep_smoke_test "
    "will build standalone for now; it links lockstep_core once A2 lands.")
endif()

add_test(NAME smoke COMMAND lockstep_smoke_test)
