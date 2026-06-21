# providers/sim — simulated providers (virtual time, fault injection, seeded).
# Placeholder: bodies arrive Phase 1-2 (Sim IClock/IScheduler/IRandom, then
# Sim INetwork/IDisk). INTERFACE lib so the tree configures cleanly with no
# sources. NOTE: providers/ is the lint-exempt dir (cardinal rule 1) — it is the
# only place nondeterministic primitives may live.
add_library(lockstep_providers_sim INTERFACE)
# // bodies arrive Phase N
