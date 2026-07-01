#pragma once
// StateHash.hpp — CROSS-REPLICA KEYSPACE HASH (plan P3 — the etcd corrupt-check primitive).
//
// Raft guarantees the committed LOG agrees across replicas; the existing cross-check
// (V-XCHECK / StateMachineSafety) verifies exactly that. But the APPLIED state — the
// materialised keyspace each replica builds by executing its committed log into a
// storage engine — can still silently diverge from its peers if a replica suffers a
// bit-flip at rest, a storage bug, or logical corruption. Log agreement cannot see
// that: the ops are identical, the RESULT is not.
//
// keyspace_hash() hashes the applied state itself: the CRC32 over the byte-identical,
// key-ascending backup image (storage/Backup.hpp, V-DET) at a commit point. Two
// replicas that applied the same committed prefix into an uncorrupted engine produce
// the SAME hash; a divergence in the materialised keyspace produces a DIFFERENT hash —
// even when every replica's committed log is identical. Comparing these hashes across
// replicas (the leader's periodic corrupt-check, and an initial check at startup before
// serving) turns silent applied-state divergence into a detectable, alarmable event.
//
// This header is the deterministic PRIMITIVE + the pure comparison. Exchanging the hash
// across replicas (an admin endpoint / leader probe) is wired on top of it.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/storage/Backup.hpp>
#include <lockstep/storage/Codec.hpp>
#include <lockstep/storage/Engine.hpp>

namespace lockstep::storage {

// A replica's applied-keyspace fingerprint at a commit point. `crc` is the CRC32 over
// the sorted (key,value) payload — a pure function of the LIVE keyspace at `at`, so it
// is independent of a replica's local Seq metadata and comparable across replicas that
// reached the same logical state. `payload_len` is a cheap secondary witness.
struct KeyspaceHash {
    std::uint32_t crc = 0;
    std::uint64_t at = 0;
    std::uint64_t payload_len = 0;
};

// Compute the keyspace hash of `e`'s live state as-of Seq `at`. Reuses the byte-identical
// backup image and reads its embedded payload CRC (bytes 24..27) — the CRC32 over the
// key-ascending (key,value) records. Drives the scan on `sched` (engine + disks share it).
[[nodiscard]] inline KeyspaceHash keyspace_hash(core::Scheduler& sched, Engine& e, Seq at) {
    std::vector<std::byte> image;
    (void)backup_engine_bytes(sched, e, Snapshot{at}, image);
    KeyspaceHash h;
    h.at = at;
    if (image.size() >= kBackupHeaderBytes) {
        h.payload_len = get_u64(image.data() + 16);
        h.crc = get_u32(image.data() + 24);
    }
    return h;
}

// One replica's reported hash (its node id + fingerprint), for the leader's comparison.
struct ReplicaHash {
    std::string node;
    KeyspaceHash hash;
};

// The witness for a CORRUPT alarm: two replicas whose applied keyspace disagrees at the
// same commit point. `ref`/`other` index into the compared vector.
struct HashDivergence {
    std::size_t ref = 0;
    std::size_t other = 0;
    std::string ref_node;
    std::string other_node;
    std::uint32_t ref_crc = 0;
    std::uint32_t other_crc = 0;
    std::uint64_t at = 0;
};

// Compare replica hashes taken AT THE SAME commit index. Returns nullopt if every replica
// agrees with the first (all-clean); otherwise the first diverging replica — the leader
// raises a CORRUPT alarm on a non-nullopt result. A crc OR payload_len mismatch counts as
// divergence (either proves the materialised keyspaces differ).
[[nodiscard]] inline std::optional<HashDivergence> find_hash_divergence(
    std::span<const ReplicaHash> replicas) {
    if (replicas.size() < 2) {
        return std::nullopt;  // nothing to compare against.
    }
    const ReplicaHash& ref = replicas[0];
    for (std::size_t i = 1; i < replicas.size(); ++i) {
        const ReplicaHash& o = replicas[i];
        if (o.hash.crc != ref.hash.crc || o.hash.payload_len != ref.hash.payload_len) {
            HashDivergence d;
            d.ref = 0;
            d.other = i;
            d.ref_node = ref.node;
            d.other_node = o.node;
            d.ref_crc = ref.hash.crc;
            d.other_crc = o.hash.crc;
            d.at = ref.hash.at;
            return d;
        }
    }
    return std::nullopt;
}

}  // namespace lockstep::storage
