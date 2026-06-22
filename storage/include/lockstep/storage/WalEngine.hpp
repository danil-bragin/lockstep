#pragma once

// WalEngine.hpp — Phase 3 §5 step 2. The FIRST real Lockstep storage engine:
// a WAL + memtable + MVCC, crash-consistent over the sim IDisk. NO SSTable yet
// (that is §5 step 4) — the entire committed history lives in the WAL on disk and
// in an in-memory memtable; recover() rebuilds the memtable from the durable WAL
// prefix. This is the smallest correct engine that passes the differential
// harness under crash faults (storage-engine.md §2 V-DUR / V-PREFIX / V-SNAP /
// V-MONO / V-NOTORN / V-DET).
//
// ----------------------------------------------------------------------------
// WRITE PATH (§3): put/del(k,v) →
//   1. assign the next monotonic Seq (V-MONO: ++, never reused, never gapped).
//   2. append a length-prefixed, CRC-tagged WAL record to the IDisk (staged,
//      not yet durable).
//   3. insert the MVCC version into the in-memory memtable.
// The commit Seq is returned immediately; the record is durable only after a
// subsequent sync() (V-DUR). A crash before sync() MAY lose the staged suffix.
//
// SYNC PATH (§3 group commit): sync() is the durability barrier — it forwards to
// IDisk::sync(), which promotes staged bytes to durable. After it returns ok,
// every record appended before the call survives a crash (V-DUR). (The sim may
// model a LYING fsync that silently keeps a tail un-durable; that tail is lost on
// the next crash — exactly the prefix contract, never a fabricated value.)
//
// RECOVER PATH (§3 / C3.8): recover() reads the durable byte image back, decodes
// WAL records front-to-back, and STOPS at the FIRST record that fails its CRC,
// decodes short (a torn write that landed a partial record), or runs past the end
// of the durable bytes. Everything BEFORE that first bad record is replayed into
// a fresh memtable → a consistent PREFIX of the committed history (V-PREFIX /
// V-NOTORN). This is the batch-2 WAL-CRC lesson reused verbatim: per-record
// integrity + stop-at-first-corrupt, so a torn/lying tail can NEVER fabricate or
// resurrect a value.
//
// ----------------------------------------------------------------------------
// WAL RECORD FORMAT (little-endian, fixed by this file; the on-disk contract):
//
//   offset  size  field
//   ------  ----  ------------------------------------------------------------
//   0       4     magic   = 0x4C57414C  ('LWAL') — frames a record start
//   4       1     type    = 0 put | 1 del(tombstone)
//   5       8     seq     = the commit Seq assigned to this record
//   13      4     klen    = key byte length
//   17      4     vlen    = value byte length (0 for a tombstone)
//   21      klen  key bytes (opaque)
//   21+klen vlen  value bytes (opaque; absent for a tombstone)
//   ...     4     crc32   = CRC32 over bytes [0 .. end-of-payload) of THIS record
//
// The trailing CRC covers the header + key + value, so ANY torn prefix (a partial
// header, a partial key, a truncated value, or a flipped byte) fails the check
// and stops replay. The magic + length fields also let a decoder detect a record
// that claims more bytes than the durable image holds (a torn tail) and stop.
//
// ----------------------------------------------------------------------------
// MEMTABLE / MVCC (C3.2 / C3.5): the in-memory store is a vector of KeyVersions,
// kept sorted by key; each key holds its versions in Seq-ASCENDING order (append
// == commit order). This MIRRORS the Oracle's structure on purpose, so for the
// same op stream the engine assigns identical Seqs and answers get(k,{at}) — the
// newest version with seq <= at, ∅ on none/tombstone — IDENTICALLY (the whole
// point of the differential gate). Snapshot reads are a pure function of (k, at):
// they never see a version > at and never observe an in-flight mutation, because
// a version is only inserted AFTER its Seq is assigned (V-SNAP). Old versions are
// retained (no GC yet), so an older snapshot still sees its value.
//
// ----------------------------------------------------------------------------
// AWAIT SAFETY (V-RKV1, carried from Phase 2): every co_await on a disk op
// resolves to a value (Error) copied into a local before we touch the memtable
// again. We NEVER hold a pointer/reference into the memtable (a growable vector)
// across a co_await — get() re-resolves the version AFTER any await, and the
// write path appends to disk (awaiting) BEFORE mutating the memtable, holding no
// memtable reference across the await. The on-disk read buffer is a local vector.
//
// FORBIDDEN (storage/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, <random>, raw file IO, unordered iteration affecting
// output. All IO is through core::IDisk; all async on the scheduler; the CRC is
// hand-rolled (no library). This whole engine is a pure function of (seed, ops).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/storage/Codec.hpp>
#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/SSTable.hpp>

namespace lockstep::storage {

using core::Error;
using core::ErrorCode;
using core::Future;
using core::IDisk;
using core::make_promise;
using core::Offset;
using core::Promise;
using core::Scheduler;

// CRC32 + put_u32/put_u64/get_u32/get_u64 now live in Codec.hpp (shared with the
// SSTable + manifest formats). They are used below verbatim.

// ---------------------------------------------------------------------------
// WalRecord — the logical content of one WAL entry (decoded form). The on-disk
// byte layout is documented at the top of this file; encode()/try_decode() are
// the ONLY (de)serialisers, so the format lives in exactly one place.
// ---------------------------------------------------------------------------
struct WalRecord {
    Seq seq = kNoSeq;
    Key key;
    Value value;
    bool tombstone = false;
};

// The fixed framing constants (the on-disk contract).
inline constexpr std::uint32_t kWalMagic = 0x4C57414Cu;  // 'LWAL'
inline constexpr std::size_t kWalHeaderBytes = 21;       // magic+type+seq+klen+vlen
inline constexpr std::size_t kWalCrcBytes = 4;

// Encode one record to its on-disk bytes (header + key + value + trailing CRC).
[[nodiscard]] inline std::vector<std::byte> encode_record(const WalRecord& r) {
    std::vector<std::byte> buf;
    buf.reserve(kWalHeaderBytes + r.key.size() + r.value.size() + kWalCrcBytes);
    put_u32(buf, kWalMagic);
    buf.push_back(static_cast<std::byte>(r.tombstone ? 1u : 0u));
    put_u64(buf, r.seq);
    put_u32(buf, static_cast<std::uint32_t>(r.key.size()));
    put_u32(buf, static_cast<std::uint32_t>(r.value.size()));
    for (char c : r.key) {
        buf.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    }
    for (char c : r.value) {
        buf.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    }
    // CRC covers the whole payload up to here; append it last.
    const std::uint32_t crc = Crc32::compute(std::span<const std::byte>(buf.data(), buf.size()));
    put_u32(buf, crc);
    return buf;
}

// The outcome of decoding one record from a byte image at `pos`.
struct DecodeResult {
    bool ok = false;        // a complete, CRC-valid record was decoded
    WalRecord record;       // valid only when ok
    std::size_t consumed = 0;  // bytes this record occupied (valid only when ok)
};

// Try to decode ONE record starting at `image[pos..]`. Returns ok=false on ANY
// of: not enough bytes for the header; bad magic; a claimed key/value length that
// runs past the end of the durable image (a torn tail); or a CRC mismatch (a
// flipped/torn byte). The recover loop treats ok=false as "stop here" — the
// consistent-prefix boundary. This is the single integrity gate (V-NOTORN).
[[nodiscard]] inline DecodeResult try_decode(const std::vector<std::byte>& image, std::size_t pos) {
    DecodeResult dr;
    const std::size_t avail = image.size() - pos;
    if (avail < kWalHeaderBytes + kWalCrcBytes) {
        return dr;  // not even a header+crc — torn/truncated tail
    }
    const std::byte* p = image.data() + pos;
    if (get_u32(p) != kWalMagic) {
        return dr;  // not a record boundary — corrupt/garbage
    }
    const std::uint8_t type = std::to_integer<std::uint8_t>(p[4]);
    if (type > 1) {
        return dr;  // unknown record type — corrupt
    }
    const std::uint64_t seq = get_u64(p + 5);
    const std::uint32_t klen = get_u32(p + 13);
    const std::uint32_t vlen = get_u32(p + 17);
    // Total record length; guard against overflow + running past the image.
    const std::uint64_t body =
        static_cast<std::uint64_t>(klen) + static_cast<std::uint64_t>(vlen);
    const std::uint64_t total =
        static_cast<std::uint64_t>(kWalHeaderBytes) + body + kWalCrcBytes;
    if (total > avail) {
        return dr;  // claims more bytes than survived — torn tail, stop.
    }
    const std::size_t record_bytes = static_cast<std::size_t>(kWalHeaderBytes + body);
    // Verify the trailing CRC over [pos, pos+record_bytes).
    const std::uint32_t want = get_u32(p + record_bytes);
    const std::uint32_t got =
        Crc32::compute(std::span<const std::byte>(p, record_bytes));
    if (want != got) {
        return dr;  // flipped/torn byte — integrity failure, stop.
    }
    // Materialise the decoded record.
    WalRecord r;
    r.seq = seq;
    r.tombstone = (type == 1);
    r.key.assign(reinterpret_cast<const char*>(p + kWalHeaderBytes), klen);
    r.value.assign(reinterpret_cast<const char*>(p + kWalHeaderBytes + klen), vlen);
    dr.ok = true;
    dr.record = std::move(r);
    dr.consumed = static_cast<std::size_t>(total);
    return dr;
}

// ---------------------------------------------------------------------------
// Memtable — the in-memory MVCC store (C3.2/C3.5). Sorted-by-key vector; each key
// holds Seq-ascending versions (tombstones included). Mirrors the Oracle exactly.
// ---------------------------------------------------------------------------
class Memtable {
public:
    struct Version {
        Seq seq = kNoSeq;
        Value value;
        bool tombstone = false;
    };
    struct KeyVersions {
        Key key;
        std::vector<Version> versions;
    };

    // Insert a version (commit order ⇒ already Seq-ascending append).
    void insert(const Key& key, Seq seq, Value value, bool tombstone) {
        KeyVersions& kv = versions_for(key);
        kv.versions.push_back(Version{seq, std::move(value), tombstone});
    }

    // MVCC read: newest version of key with seq <= at; ∅ if none or it is a
    // tombstone. Pure function of (key, at) — identical rule to the Oracle.
    [[nodiscard]] std::optional<Value> lookup(const Key& key, Seq at) const {
        const Hit h = lookup_hit(key, at);
        if (!h.covered || h.tombstone) {
            return std::nullopt;
        }
        return h.value;
    }

    // The newest version of key with seq <= at, distinguishing "covered (incl. a
    // tombstone)" from "no version <= at at all". The LSM read path needs this so
    // a memtable tombstone correctly SHADOWS an older SSTable value (returns ∅)
    // rather than being mistaken for "memtable has nothing, fall through".
    struct Hit {
        bool covered = false;
        bool tombstone = false;
        Value value;
        Seq seq = kNoSeq;
    };
    [[nodiscard]] Hit lookup_hit(const Key& key, Seq at) const {
        Hit hit;
        const KeyVersions* kv = find(key);
        if (kv == nullptr) {
            return hit;
        }
        const Version* newest = nullptr;
        for (const Version& v : kv->versions) {
            if (v.seq <= at) {
                newest = &v;  // ascending ⇒ later hits are strictly newer
            } else {
                break;
            }
        }
        if (newest == nullptr) {
            return hit;
        }
        hit.covered = true;
        hit.tombstone = newest->tombstone;
        hit.value = newest->value;
        hit.seq = newest->seq;
        return hit;
    }

    void clear() { keys_.clear(); }

    // Total version count across all keys (the flush-threshold metric).
    [[nodiscard]] std::size_t version_count() const noexcept {
        std::size_t n = 0;
        for (const KeyVersions& kv : keys_) {
            n += kv.versions.size();
        }
        return n;
    }

    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }

    // Read-only access to the sorted keys (for flush serialisation + scan merge).
    [[nodiscard]] const std::vector<KeyVersions>& keys() const noexcept { return keys_; }

private:
    [[nodiscard]] const KeyVersions* find(const Key& key) const {
        // keys_ sorted ⇒ a binary search would do; a linear scan is fine + clear.
        for (const KeyVersions& kv : keys_) {
            if (kv.key == key) {
                return &kv;
            }
            if (key < kv.key) {
                break;  // sorted: past where it would be
            }
        }
        return nullptr;
    }

    KeyVersions& versions_for(const Key& key) {
        std::size_t pos = 0;
        while (pos < keys_.size() && keys_[pos].key < key) {
            ++pos;
        }
        if (pos < keys_.size() && keys_[pos].key == key) {
            return keys_[pos];
        }
        KeyVersions kv;
        kv.key = key;
        keys_.insert(keys_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(kv));
        return keys_[pos];
    }

    std::vector<KeyVersions> keys_;  // sorted by key; each list Seq-ascending
};

// ---------------------------------------------------------------------------
// IDiskFactory — mints / returns the append-structured IDisk backing one SSTable.
// Each SSTable is its own IDisk (the IDisk contract: one append-structured object
// per handle). The factory keys on a stable sstable_id so that, after a crash,
// recovery can re-open the SAME disk for a manifest-referenced SSTable. The sim
// test owns a concrete factory holding a pool of SimDisks; the engine only ever
// talks to the returned IDisk via append/read/sync (no bare syscalls).
// ---------------------------------------------------------------------------
class IDiskFactory {
public:
    virtual ~IDiskFactory() = default;
    // Return the IDisk for `sstable_id`, creating it on first request. Stable for
    // the lifetime of the run so recovery re-opens the same backing.
    [[nodiscard]] virtual IDisk& disk_for(std::uint64_t sstable_id) = 0;
};

// ---------------------------------------------------------------------------
// WalEngine — the concrete Engine (Engine.hpp) over WAL + memtable + MVCC, now
// with SSTable flush + a newest→oldest LSM read/scan path (Phase 3 §5 step 4).
//
// Two construction modes (backward compatible):
//   * WalEngine(sched, disk)                       — WAL + memtable ONLY (step 2;
//     flush disabled, the whole history lives in the memtable + WAL).
//   * WalEngine(sched, wal, manifest, factory, T)  — LSM: when the memtable holds
//     > T versions, flush() serialises it to a NEW SSTable (its own IDisk),
//     syncs it, then atomically INSTALLS it via a CRC'd, Seq-contiguous manifest
//     record (synced) — only then are the flushed versions dropped from the
//     memtable. A crash mid-flush leaves either the old state (manifest record
//     never landed) or a complete, integrity-checked, manifest-committed SSTable.
//     The WAL is NOT truncated here (that is compaction's job): recovery replays
//     the full WAL prefix OVER the manifest's SSTable set, and the read path
//     merges memtable + SSTables newest-version-per-key, so a version present in
//     both is harmless.
// ---------------------------------------------------------------------------
class WalEngine final : public Engine {
public:
    // Step-2 mode: WAL + memtable only (no SSTable flush).
    WalEngine(Scheduler& sched, IDisk& disk) noexcept : sched_(&sched), disk_(&disk) {}

    // LSM mode: WAL disk + manifest disk + SSTable disk factory + flush threshold
    // (in memtable version count). Set threshold to 0 to disable flush.
    WalEngine(Scheduler& sched, IDisk& wal_disk, IDisk& manifest_disk,
              IDiskFactory& factory, std::size_t flush_threshold) noexcept
        : sched_(&sched),
          disk_(&wal_disk),
          manifest_disk_(&manifest_disk),
          factory_(&factory),
          flush_threshold_(flush_threshold) {}

    [[nodiscard]] Future<Seq> put(Key key, Value value) override {
        return commit(std::move(key), std::move(value), /*tombstone=*/false);
    }

    [[nodiscard]] Future<Seq> del(Key key) override {
        return commit(std::move(key), Value{}, /*tombstone=*/true);
    }

    [[nodiscard]] Future<std::optional<Value>> get(Key key, Snapshot snap) override {
        // LSM read path (§3): the memtable holds the NEWEST (un-flushed) versions;
        // SSTables hold older flushed versions. We find the newest version with
        // seq <= snap.at across BOTH, memtable first then SSTables newest→oldest.
        // The first source that COVERS the key with the highest such seq wins.
        // V-SNAP: never a version > snap.at; pure fn of (key, at).
        Promise<std::optional<Value>> p = make_promise<std::optional<Value>>(sched_);
        Future<std::optional<Value>> f = p.get_future();

        // Find the version with the MAXIMUM seq <= snap.at across the memtable AND
        // every SSTable, and use it. We do NOT assume "memtable is always newest":
        // after a crash, a lying-fsync-truncated WAL can leave the memtable holding
        // an OLDER version than a durably-installed SSTable holds for the same key.
        // So the read MERGES BY SEQ — the highest qualifying seq wins (it is the
        // true newest version <= at), then a tombstone at that seq ⇒ ∅. (In normal
        // operation the memtable IS newest, so this still picks it.)
        Seq best_seq = kNoSeq;
        bool best_tomb = false;
        Value best_val;
        bool found = false;

        const Memtable::Hit mh = mem_.lookup_hit(key, snap.at);
        if (mh.covered && (!found || mh.seq >= best_seq)) {
            best_seq = mh.seq;
            best_tomb = mh.tombstone;
            best_val = mh.value;
            found = true;
        }
        for (const std::unique_ptr<SSTableReader>& sst : sstables_) {
            const SSTableReader::Hit h = sst->lookup(key, snap.at);
            if (h.covered && (!found || h.seq >= best_seq)) {
                best_seq = h.seq;
                best_tomb = h.tombstone;
                best_val = h.value;
                found = true;
            }
        }
        if (!found || best_tomb) {
            p.set_value(std::nullopt);
        } else {
            p.set_value(std::optional<Value>(best_val));
        }
        return f;
    }

    [[nodiscard]] Future<Snapshot> snapshot() override {
        Promise<Snapshot> p = make_promise<Snapshot>(sched_);
        Future<Snapshot> f = p.get_future();
        p.set_value(Snapshot{last_seq_});
        return f;
    }

    [[nodiscard]] Future<Error> sync() override {
        // The durability barrier: forward to the disk. After ok, every record
        // appended before this survives a crash (V-DUR). A lying fsync keeps a
        // tail un-durable — lost on crash, never fabricated (V-PREFIX).
        return disk_->sync();
    }

    [[nodiscard]] Future<std::vector<KeyValue>> scan(Range range, Snapshot snap) override {
        // LSM range read (§1/§5): merge the memtable + every SSTable at the
        // snapshot, newest-version-per-key, dropping tombstones, key-ascending.
        // All sources are in-memory after load → no await. Pure fn of (range, at).
        Promise<std::vector<KeyValue>> p = make_promise<std::vector<KeyValue>>(sched_);
        Future<std::vector<KeyValue>> f = p.get_future();

        // Collect, per key, the NEWEST (seq, value, tombstone) with seq <= at,
        // across the memtable and all SSTables. The memtable + a later-installed
        // SSTable both supply candidates; we keep the highest seq per key.
        struct Best {
            Seq seq = kNoSeq;
            Value value;
            bool tombstone = false;
            bool present = false;
        };
        // Keep candidates in a sorted vector keyed by Key (deterministic, no hash).
        std::vector<std::pair<Key, Best>> merged;
        auto offer = [&](const Key& k, Seq s, const Value& v, bool tomb) {
            // Insert/merge in sorted position; keep the highest seq.
            std::size_t i = 0;
            while (i < merged.size() && merged[i].first < k) {
                ++i;
            }
            if (i < merged.size() && merged[i].first == k) {
                if (s >= merged[i].second.seq) {
                    merged[i].second = Best{s, v, tomb, true};
                }
                return;
            }
            merged.insert(merged.begin() + static_cast<std::ptrdiff_t>(i),
                          std::pair<Key, Best>{k, Best{s, v, tomb, true}});
        };

        auto in_range = [&](const Key& k) {
            if (k < range.lo) {
                return false;
            }
            if (!range.hi_unbounded && !(k < range.hi)) {
                return false;
            }
            return true;
        };

        // Memtable candidates: for each key, its newest version <= at.
        for (const Memtable::KeyVersions& kv : mem_.keys()) {
            if (!in_range(kv.key)) {
                continue;
            }
            const Memtable::Hit h = mem_.lookup_hit(kv.key, snap.at);
            if (h.covered) {
                offer(kv.key, h.seq, h.value, h.tombstone);
            }
        }
        // SSTable candidates (each contributes every entry in range <= at; the
        // offer() merge keeps the newest seq per key across all sources).
        for (const std::unique_ptr<SSTableReader>& sst : sstables_) {
            std::vector<std::pair<Key, SSTableReader::ScanCand>> acc;
            const Key hi = range.hi_unbounded ? Key{} : range.hi;
            sst->scan_into(range.lo, hi, snap.at, range.hi_unbounded, acc);
            for (const auto& c : acc) {
                offer(c.first, c.second.seq, c.second.value, c.second.tombstone);
            }
        }

        std::vector<KeyValue> out;
        for (const auto& m : merged) {
            if (m.second.present && !m.second.tombstone) {
                out.emplace_back(m.first, m.second.value);
            }
        }
        p.set_value(std::move(out));
        return f;
    }

    // ---- crash recovery (C3.8) — sim-only entry, not part of Engine.hpp -----
    //
    // Rebuild the memtable from the durable WAL prefix. Reads the durable byte
    // image back, decodes records front-to-back, STOPS at the first that fails
    // integrity/decoding → a consistent prefix. last_seq_ is set to the last
    // replayed record's Seq (V-MONO continues from there). Returns a Future so it
    // runs on the scheduler like every other async op.
    [[nodiscard]] Future<Error> recover(std::size_t durable_len) {
        Promise<Error> p = make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        sched_->spawn(recover_task(std::move(p), durable_len, /*manifest_len=*/0));
        return f;
    }

    // LSM recover: load the manifest's committed SSTable set (manifest_len durable
    // manifest bytes) THEN replay the WAL prefix (wal_len durable WAL bytes). The
    // manifest is replayed stop-at-first-corrupt AND stop-at-gap (Seq-contiguity
    // on entry_no): a torn manifest tail can never install a half-flushed SSTable
    // (a valid SSTable block CRC does NOT make it live without a committed
    // manifest record). Each referenced SSTable is loaded + fully CRC-validated;
    // a rejected SSTable is treated as not-installed (its data, if it predates the
    // crash, is still in the WAL prefix). Reads then merge memtable + SSTables.
    [[nodiscard]] Future<Error> recover_lsm(std::size_t wal_len, std::size_t manifest_len) {
        Promise<Error> p = make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        sched_->spawn(recover_task(std::move(p), wal_len, manifest_len));
        return f;
    }

    // Force an immediate flush of the current memtable to a new SSTable (test +
    // metamorphic hook; sim-only, not part of Engine.hpp). Crash-safe (atomic
    // install). A no-op in WAL-only mode or with an empty memtable.
    [[nodiscard]] Future<Error> force_flush() {
        Promise<Error> p = make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        sched_->spawn(force_flush_task(std::move(p)));
        return f;
    }

    [[nodiscard]] Seq last_seq() const noexcept { return last_seq_; }
    [[nodiscard]] std::size_t sstable_count() const noexcept { return sstables_.size(); }

private:
    // The write path: assign Seq, append the CRC-tagged record (awaiting the
    // disk), then insert the MVCC version. The Seq is assigned UP FRONT and is
    // monotonic (V-MONO). We hold NO memtable reference across the co_await — the
    // append's payload is an owned byte vector and the memtable insert happens
    // only after the await resolves to a value (V-RKV1).
    [[nodiscard]] Future<Seq> commit(Key key, Value value, bool tombstone) {
        const Seq seq = ++last_seq_;  // 1,2,3,... — 0 stays the ∅ sentinel.
        Promise<Seq> p = make_promise<Seq>(sched_);
        Future<Seq> f = p.get_future();
        sched_->spawn(commit_task(std::move(p), seq, std::move(key), std::move(value),
                                  tombstone));
        return f;
    }

    core::Task commit_task(Promise<Seq> p, Seq seq, Key key, Value value, bool tombstone) {
        WalRecord rec;
        rec.seq = seq;
        rec.tombstone = tombstone;
        rec.key = key;            // keep a copy for the post-await memtable insert
        rec.value = value;
        const std::vector<std::byte> bytes = encode_record(rec);

        Offset off = 0;
        // Await the disk append. The Error is the co_await value (set_value path).
        const Error e =
            co_await disk_->append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
        (void)e;  // a torn/io-faulted append is fine: recovery's CRC catches it;
                  // the in-memory memtable still reflects the (pre-durability)
                  // commit, and a crash before sync drops it cleanly (V-PREFIX).

        // Insert AFTER the await — we held no memtable reference across it.
        mem_.insert(key, seq, std::move(value), tombstone);

        // Flush BEFORE returning the commit Seq if the memtable is over the
        // threshold AND we are in LSM mode. Flush is crash-safe (atomic-install
        // via the manifest); doing it inline here keeps the engine single-flow
        // (no background thread; everything on the scheduler). The commit's Seq is
        // already assigned + WAL-durable-on-next-sync, so flushing now never loses
        // it. A flush failure (io fault) leaves the memtable intact — the versions
        // remain readable from memory + WAL; the next flush retries.
        if (flush_threshold_ > 0 && mem_.version_count() > flush_threshold_) {
            co_await do_flush();
        }
        p.set_value(seq);
        co_return;
    }

    core::Task force_flush_task(Promise<Error> p) {
        co_await do_flush();
        p.set_value(Error{});
        co_return;
    }

    // ---- FLUSH (C3.3) — serialise the memtable to a new, crash-safe SSTable ---
    //
    // Discipline (atomic install, V-NOTORN / V-PREFIX):
    //   1. Snapshot the memtable into a key-/seq-ascending entry run + its key set.
    //   2. Serialise to the SSTable byte image (data blocks w/ per-block CRC +
    //      sparse index + bloom + CRC'd footer).
    //   3. Append the image to a NEW SSTable IDisk and SYNC it (durable bytes).
    //   4. Append a CRC'd, Seq-contiguous manifest record (entry_no, sstable_id,
    //      sst_len, min/max seq) and SYNC the manifest. ONLY NOW is the SSTable
    //      "live". A crash before this sync leaves the SSTable un-referenced →
    //      invisible on recovery (the bytes are harmless; the data is still in the
    //      WAL). A crash after it → the SSTable is committed + integrity-checked.
    //   5. Install the in-memory reader and DROP the flushed versions from the
    //      memtable. (The WAL is NOT truncated — recovery replays the full WAL
    //      prefix over the manifest's SSTable set; duplicate (key,seq) is benign.)
    core::Task do_flush() {
        if (manifest_disk_ == nullptr || factory_ == nullptr || mem_.empty()) {
            co_return;
        }
        // (1) snapshot the memtable → entries (key-asc, seq-asc — its native order).
        std::vector<SstEntry> entries;
        for (const Memtable::KeyVersions& kv : mem_.keys()) {
            for (const Memtable::Version& v : kv.versions) {
                entries.push_back(SstEntry{kv.key, v.value, v.seq, v.tombstone});
            }
        }
        if (entries.empty()) {
            co_return;
        }

        // (2) serialise.
        const SstBuildResult built = SSTableBuilder::build(entries);
        const std::uint64_t sstable_id = next_sstable_id_++;
        IDisk& sdisk = factory_->disk_for(sstable_id);

        // (3) append image + sync the SSTable disk. The image is an owned local;
        // we hold no memtable reference across the awaits (V-RKV1).
        Offset off = 0;
        const Error ae = co_await sdisk.append(
            std::span<const std::byte>(built.bytes.data(), built.bytes.size()), off);
        if (!ae.ok()) {
            // The SSTable image did not fully land. Abort the flush: leave the
            // memtable untouched (data still in memory + WAL); no manifest record.
            co_return;
        }
        const Error se = co_await sdisk.sync();
        if (!se.ok()) {
            co_return;
        }

        // (4) commit the install via a CRC'd, Seq-contiguous manifest record, then
        // sync the manifest. The record references the EXACT durable length
        // (built.bytes.size()), so a torn tail past it is never read on recovery.
        ManifestRecord mr;
        mr.entry_no = ++manifest_tip_;       // 1,2,3,… contiguous install order
        mr.sstable_id = sstable_id;
        mr.sst_len = built.bytes.size();
        mr.min_seq = built.min_seq;
        mr.max_seq = built.max_seq;
        const std::vector<std::byte> mbytes = encode_manifest(mr);
        Offset moff = 0;
        const Error me = co_await manifest_disk_->append(
            std::span<const std::byte>(mbytes.data(), mbytes.size()), moff);
        if (!me.ok()) {
            --manifest_tip_;  // the record did not land — roll back the install no.
            co_return;
        }
        const Error mse = co_await manifest_disk_->sync();
        if (!mse.ok()) {
            --manifest_tip_;
            co_return;
        }

        // (5) the SSTable is now durably installed. Build its in-memory reader and
        // drop the flushed versions from the memtable. We re-parse the just-built
        // image so the live reader uses the SAME validated path as recovery.
        auto reader = std::make_unique<SSTableReader>();
        if (SSTableLoader::parse(built.bytes, sstable_id, *reader)) {
            sstables_.push_back(std::move(reader));
        }
        mem_.clear();  // everything was flushed (we snapshot the WHOLE memtable).
        co_return;
    }

    core::Task recover_task(Promise<Error> p, std::size_t durable_len,
                            std::size_t manifest_len) {
        // Fresh recovery state.
        mem_.clear();
        sstables_.clear();
        last_seq_ = kNoSeq;
        manifest_tip_ = 0;
        next_sstable_id_ = 0;

        // (0) LSM: replay the manifest to load the committed SSTable set FIRST.
        // Stop-at-first-corrupt AND stop-at-gap on entry_no (Seq-contiguity).
        if (manifest_len > 0 && manifest_disk_ != nullptr && factory_ != nullptr) {
            std::vector<std::byte> man(manifest_len);
            const Error mre =
                co_await manifest_disk_->read(0, std::span<std::byte>(man.data(), man.size()));
            if (mre.ok()) {
                std::size_t mpos = 0;
                std::uint64_t expect_entry = 1;
                while (mpos < man.size()) {
                    const ManifestDecode md = decode_manifest(man, mpos);
                    if (!md.ok) {
                        break;  // torn/corrupt manifest tail → install-prefix end.
                    }
                    if (md.record.entry_no != expect_entry) {
                        break;  // a gap in install order → stop (Seq-contiguity).
                    }
                    // Load + fully validate the referenced SSTable. A rejected
                    // SSTable (torn/corrupt) is skipped — NOT installed.
                    IDisk& sdisk = factory_->disk_for(md.record.sstable_id);
                    auto reader = std::make_unique<SSTableReader>();
                    Error lerr{};
                    co_await SSTableLoader::load(sdisk, md.record.sst_len,
                                                 md.record.sstable_id, *reader, lerr);
                    if (!lerr.ok()) {
                        // The manifest record is valid but its SSTable failed
                        // integrity (torn/corrupt bytes). STOP the install prefix
                        // here — a later SSTable must NOT load past a missing
                        // earlier one (that would leave a Seq GAP inside the
                        // prefix, the exact stop-at-first-corrupt + Seq-contiguity
                        // lesson applied at the SSTable granularity). The dropped
                        // SSTable's data, if it predates the crash, is still in the
                        // WAL prefix.
                        break;
                    }
                    // An installed SSTable's versions are durably committed: its
                    // max_seq is part of the recovered prefix tip even if a lying-
                    // fsync truncated the WAL below it. In-order flushes cover a
                    // contiguous [1, max_sst] prefix.
                    if (reader->max_seq > last_seq_) {
                        last_seq_ = reader->max_seq;
                    }
                    sstables_.push_back(std::move(reader));
                    // Track install state so post-recovery flushes continue the
                    // contiguous entry numbering + non-colliding sstable ids.
                    manifest_tip_ = md.record.entry_no;
                    if (md.record.sstable_id + 1 > next_sstable_id_) {
                        next_sstable_id_ = md.record.sstable_id + 1;
                    }
                    mpos += md.consumed;
                }
            }
        }

        // (1) replay the WAL prefix into the memtable (over the SSTable set).
        std::vector<std::byte> image(durable_len);
        if (durable_len > 0) {
            const Error re =
                co_await disk_->read(0, std::span<std::byte>(image.data(), image.size()));
            if (!re.ok()) {
                // A read fault / corruption covering the WAL prefix: we cannot
                // trust the WAL, but the manifest's committed SSTables ARE a valid
                // durable prefix — keep them (last_seq_ already reflects their max,
                // mem_ empty). Recover to "SSTables only" rather than fabricate.
                mem_.clear();
                p.set_value(Error{});
                co_return;
            }
        }

        // Decode + replay front-to-back; stop at the first integrity failure. NOTE
        // last_seq_ already holds the max committed SSTable seq (a durable prefix);
        // the WAL prefix extends it. We do NOT reset it to 0 here.
        mem_.clear();
        std::size_t pos = 0;
        Seq expect = kNoSeq + 1;  // the first valid commit Seq is 1.
        while (pos < image.size()) {
            const DecodeResult dr = try_decode(image, pos);
            if (!dr.ok) {
                break;  // first corrupt/torn record → consistent-prefix boundary.
            }
            // SEQ-CONTIGUITY GUARD (V-PREFIX): a torn/io-faulted append can drop a
            // record from the MIDDLE of the log while LATER records still land —
            // leaving a GAP. A gap is a lost commit; replaying past it would
            // resurrect a non-prefix (a hole that hides a lost mutation). So if
            // the next decoded record is not exactly the expected next Seq, STOP:
            // the consistent prefix ends at the gap. (Without this guard the
            // memtable would skip the lost Seq and surface an older version where
            // the truth is the lost newer one — exactly the seed-1 k4 case.)
            if (dr.record.seq != expect) {
                break;
            }
            mem_.insert(dr.record.key, dr.record.seq, dr.record.value,
                        dr.record.tombstone);
            // The WAL prefix extends the tip; never lower it below an installed
            // SSTable's max (the WAL replays from seq 1 upward, SSTable seqs may
            // already have raised last_seq_ above the current WAL position).
            if (dr.record.seq > last_seq_) {
                last_seq_ = dr.record.seq;
            }
            ++expect;
            pos += dr.consumed;
        }
        p.set_value(Error{});
        co_return;
    }

    Scheduler* sched_;
    IDisk* disk_;
    Memtable mem_;
    Seq last_seq_ = kNoSeq;

    // ---- LSM state (null/empty in step-2 WAL-only mode) -------------------
    IDisk* manifest_disk_ = nullptr;          // the atomic-install log backing
    IDiskFactory* factory_ = nullptr;         // mints per-SSTable IDisks
    std::size_t flush_threshold_ = 0;         // memtable version count to flush at
    std::vector<std::unique_ptr<SSTableReader>> sstables_;  // install order (old→new)
    std::uint64_t next_sstable_id_ = 0;       // next SSTable backing id
    std::uint64_t manifest_tip_ = 0;          // last committed manifest entry_no
};

}  // namespace lockstep::storage
