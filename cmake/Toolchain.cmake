# cmake/Toolchain.cmake — pin the C++ standard for the whole tree.
#
# Lockstep is C++23 everywhere. The standard is REQUIRED (no silent downgrade)
# and compiler EXTENSIONS are OFF (we want -std=c++23, never -std=gnu++23) so
# that builds are portable and we never accidentally rely on a GNU extension.
#
# Include this once from the root CMakeLists before any target is defined.

set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard for all Lockstep targets")
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "Fail if C++23 is unavailable")
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "No compiler extensions (-std=c++23, not gnu++23)")

# Emit compile_commands.json so clang-tidy / clang-format / editors can find
# the exact flags each translation unit is built with.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json")
