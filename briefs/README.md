# briefs/ — historical dispatch records

These files are **frozen, point-in-time work orders** — the actual per-phase
briefs fanned out to agents at each phase's kickoff. They are saturated with
"DISPATCH NOW" staging, hardcoded approval dates, and pinned commit hashes, and
they deliberately describe scope as it stood *at that moment* (e.g. features
later built are listed as "out of scope / deferred").

**They are an immutable audit trail, not current marching orders or a
current-state map.** Read them as history. For what the system actually is now,
see the root `README.md`, the `specs/` companion docs, and
`query/sql/SQL_FEATURES_PLAN.md`. They are intentionally left unedited; rewriting
them to "current state" would destroy their value as a record of what was ordered
and when.
