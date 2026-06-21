# providers/sim — simulated providers (virtual time, fault injection, seeded).
# Phase 1 lands SeededRandom (C1.5): the single seeded PRNG implementing IRandom.
# Header-only (templated/inline), so this stays an INTERFACE lib. NOTE: providers/
# is the lint-exempt dir (cardinal rule 1) — the only place nondeterministic
# primitives may live; SeededRandom's hand-rolled splitmix64 lives here.
add_library(lockstep_providers_sim INTERFACE)
target_include_directories(lockstep_providers_sim INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(lockstep_providers_sim INTERFACE lockstep_core)
target_compile_features(lockstep_providers_sim INTERFACE cxx_std_23)
# // sim INetwork/IDisk bodies arrive Phase 2
