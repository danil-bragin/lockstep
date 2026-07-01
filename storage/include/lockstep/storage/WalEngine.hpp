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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
#include <lockstep/storage/ValueLog.hpp>

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
    Value value;       // inline value, OR (when vlog==true) the encoded VlogPtr
    bool tombstone = false;
    bool vlog = false;  // WiscKey: `value` holds a 16-byte VlogPtr, not the value
};

// One committed logical op exported for a point-in-time (PITR) archive: the commit
// Seq, the key, and the INLINE value. A WiscKey vlog pointer is derefed to its
// value at export time, so the archive stays self-contained / portable (like a
// logical backup). `vlog` is transient bookkeeping used only while gathering.
struct ExportedOp {
    Seq seq = kNoSeq;
    Key key;
    Value value;
    bool tombstone = false;
    bool vlog = false;
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
    // type: 0 = inline put | 1 = del (tombstone) | 2 = put with a vlog pointer
    // (WiscKey large-value separation — `value` holds a fixed 16-byte VlogPtr).
    const std::uint8_t type = r.tombstone ? 1u : (r.vlog ? 2u : 0u);
    buf.push_back(static_cast<std::byte>(type));
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
    if (type > 2) {
        return dr;  // unknown record type — corrupt (0=put,1=del,2=vlog-ptr put)
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
    r.vlog = (type == 2);
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
        Value value;       // inline value, OR (vlog==true) the encoded VlogPtr
        bool tombstone = false;
        bool vlog = false;  // WiscKey: `value` is a VlogPtr, deref on read
    };
    using Versions = std::vector<Version>;  // a key's MVCC version list (Seq-ascending)

    // Insert a version (commit order ⇒ already Seq-ascending append). map_[key] is O(log N) with NO
    // element shift (the prior sorted-vector did an O(N) insert shift for every NEW key, which made a
    // columnar flush super-linear — block keys sort before the N persistent delta keys and shifted
    // them all). An ordered map keeps the SORTED iteration the scan / SSTable flush rely on, so the
    // observable key order + MVCC version lists are byte-identical (V-DET unaffected).
    void insert(const Key& key, Seq seq, Value value, bool tombstone, bool vlog = false) {
        map_[key].push_back(Version{seq, std::move(value), tombstone, vlog});
        ++total_versions_;  // running count: version_count() is O(1), not an O(N) scan per put
        if (!is_resident(key)) {
            ++flushable_versions_;  // versions of FLUSH-ELIGIBLE keys (the threshold metric)
        }
    }

    // SELECTIVE FLUSH (columnar/LSM composition): mark a leading key-byte as
    // "keep-resident" — versions of any key starting with `b` are NEVER flushed to
    // an SSTable; the higher layer (the SQL columnar engine) manages those key
    // namespaces ('B'/'M'/'R'/'T'/'Z'/'d' block/overlay/delta) itself, doing its own
    // bulk rewrites, so LSM-flushing them only churns. With NO byte marked resident
    // (the default) every key is flushable and the flush is the original
    // whole-memtable flush (byte-identical). Set before the first insert.
    void set_resident_byte(unsigned char b) noexcept {
        resident_[b] = true;
        any_resident_ = true;
    }
    [[nodiscard]] bool any_resident() const noexcept { return any_resident_; }
    [[nodiscard]] bool is_resident(const Key& key) const noexcept {
        return any_resident_ && !key.empty() &&
               resident_[static_cast<unsigned char>(key[0])];
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
        Value value;       // inline value, OR (vlog==true) the encoded VlogPtr
        Seq seq = kNoSeq;
        bool vlog = false;  // WiscKey: `value` is a VlogPtr — the caller derefs
    };
    // The newest version <= `at` from an already-located version list (the scan path holds the map
    // iterator, so it folds directly without a second lookup).
    [[nodiscard]] static Hit hit_from(const Versions& versions, Seq at) {
        Hit hit;
        const Version* newest = nullptr;
        for (const Version& v : versions) {
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
        hit.vlog = newest->vlog;
        return hit;
    }
    [[nodiscard]] Hit lookup_hit(const Key& key, Seq at) const {
        const auto it = map_.find(key);
        if (it == map_.end()) {
            return Hit{};
        }
        return hit_from(it->second, at);
    }

    void clear() {
        map_.clear();
        total_versions_ = 0;
        flushable_versions_ = 0;
    }

    // Erase every FLUSH-ELIGIBLE (non-resident) key, keeping resident keys in place.
    // Called after a selective flush: only the just-flushed (non-resident) versions
    // leave the memtable; the columnar-managed resident namespaces stay. With no
    // resident byte set this is exactly clear() (the whole-memtable flush —
    // byte-identical). O(M) over the M keys (std::map node erase, no shift).
    void erase_flushable() {
        if (!any_resident_) {
            clear();
            return;
        }
        for (auto it = map_.begin(); it != map_.end();) {
            if (it->first.empty() ||
                !resident_[static_cast<unsigned char>(it->first[0])]) {
                total_versions_ -= it->second.size();
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
        flushable_versions_ = 0;  // every flushable key was just erased
    }

    // Total version count across all keys (compaction/GC accounting) — O(1) via a running counter.
    [[nodiscard]] std::size_t version_count() const noexcept { return total_versions_; }

    // Versions of FLUSH-ELIGIBLE (non-resident) keys only — the flush-threshold metric. Without
    // selective flush this equals version_count(). It is checked on EVERY put, so it MUST stay O(1)
    // (an O(N) scan makes a bulk load O(N^2) once LSM flushing is on). Counting only flushable keys
    // is what makes selective flush converge: a flush drives this back to 0, so it does NOT re-trip
    // every put once the resident (columnar) versions alone exceed the threshold.
    [[nodiscard]] std::size_t flushable_version_count() const noexcept { return flushable_versions_; }

    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }

    // Read-only access to the sorted (key -> version list) map (for flush serialisation + scan
    // merge). std::map iterates in ascending key order — the same order the sorted vector had.
    [[nodiscard]] const std::map<Key, Versions>& entries() const noexcept { return map_; }

private:
    std::map<Key, Versions> map_;  // sorted by key; each version list Seq-ascending
    std::size_t total_versions_ = 0;  // running Σ versions (version_count() O(1))
    std::size_t flushable_versions_ = 0;  // running Σ versions of NON-resident keys (threshold metric)
    std::array<bool, 256> resident_{};  // leading bytes kept resident (never flushed)
    bool any_resident_ = false;  // fast path: skip the resident check entirely when unset
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

    // Reclaim the backing disk for an OBSOLETED SSTable (compaction superseded it;
    // its manifest OBSOLETE record is durable). The default is a no-op (the disk
    // simply stops being referenced); a sim factory may zero/free its bytes so the
    // determinism fingerprint reflects the reclaim. Called AFTER the obsolete
    // record is durable, so a crash before reclaim just re-obsoletes on recovery.
    virtual void reclaim(std::uint64_t /*sstable_id*/) {}
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

    // Set the SIZE-TIERED compaction trigger: when the LIVE SSTable count reaches
    // `n` (n>=2), the next flush merges them all into one. 0 disables compaction.
    void set_compaction_trigger(std::size_t n) noexcept { compaction_trigger_ = n; }

    // SELECTIVE FLUSH: mark each leading byte in `prefixes` as keep-resident — keys
    // in those namespaces stay in the memtable forever (never flushed to an SSTable),
    // because a higher layer manages their bulk lifecycle (the SQL columnar engine's
    // 'B'/'M'/'R'/'T'/'Z'/'d' blocks/overlays/delta). LSM flush then bounds ONLY the
    // remaining (row-mode / index) keys — the actual OLTP memtable — while columnar
    // keys behave exactly as in the no-LSM build. Recovery stays correct because, with
    // resident keys present, WAL truncation is DISABLED (compaction never advances
    // wal_trunc_seq_) so the full WAL replays the resident keys back (they are never
    // SSTable-covered). Set before the first put.
    void set_keep_resident_prefixes(std::string_view prefixes) noexcept {
        for (const char c : prefixes) {
            mem_.set_resident_byte(static_cast<unsigned char>(c));
        }
    }

    // Enable WiscKey LARGE-VALUE SEPARATION (C3.6): a value whose byte length is
    // STRICTLY GREATER than `threshold` is written to the value log and the LSM
    // stores only a 16-byte pointer. `vlog_base_id` is the IDiskFactory id of the
    // FIRST vlog generation (compaction rewrites live values into the NEXT id and
    // reclaims the old one); it lives in a disjoint id range from SSTable ids so
    // the two never collide on the factory. threshold 0 disables separation (every
    // value stays inline — the pre-WiscKey behaviour). Requires LSM mode (factory).
    void set_value_log(std::size_t threshold,
                       std::uint64_t vlog_base_id = kDefaultVlogBaseId) noexcept {
        value_threshold_ = threshold;
        vlog_base_id_ = vlog_base_id;
        if (vlog_gen_ < vlog_base_id_) {
            vlog_gen_ = vlog_base_id_;
        }
    }

    // The id range vlogs occupy on the IDiskFactory — high so it never collides
    // with SSTable ids (which start at 0). A test's fingerprint/reclaim covers it.
    static constexpr std::uint64_t kDefaultVlogBaseId = 1ull << 32;

    // Bounded retries for a TRANSIENT vlog read io-fault (a durable value that the
    // sim's per-op fault coin flipped). The durable bytes exist; a retry resolves.
    static constexpr int kVlogReadRetries = 32;

    // Set the GC READ WATERMARK (V-GC): the oldest live snapshot Seq — no live
    // snapshot reads as-of a version below this. Compaction may then drop any
    // version of a key that is shadowed by a newer version with seq <= watermark
    // (it can never be observed by a live snapshot). DEFAULT 0 ⇒ nothing is
    // droppable by the snapshot rule (every historical version is conservatively
    // retained). A caller that knows its oldest live snapshot raises this so
    // compaction reclaims truly-dead versions. The watermark is monotonic: a
    // request below the current value is ignored (a live snapshot never gets
    // older). It is clamped to the committed tip so it never exceeds reality.
    void set_read_watermark(Seq w) noexcept {
        if (w > last_seq_) {
            w = last_seq_;
        }
        if (w > read_watermark_) {
            read_watermark_ = w;
        }
    }
    [[nodiscard]] Seq read_watermark() const noexcept { return read_watermark_; }

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
        // V-SNAP: never a version > snap.at; pure fn of (key, at). WiscKey: when the
        // winning version is a vlog-pointer, DEREF it against the vlog (async read +
        // CRC verify) — so get() is a coroutine on the scheduler.
        Promise<std::optional<Value>> p = make_promise<std::optional<Value>>(sched_);
        Future<std::optional<Value>> f = p.get_future();
        sched_->spawn(get_task(std::move(p), std::move(key), snap.at));
        return f;
    }

    core::Task get_task(Promise<std::optional<Value>> p, Key key, Seq at) {
        // Find the version with the MAXIMUM seq <= at across the memtable AND every
        // SSTable. We do NOT assume "memtable is always newest": after a crash a
        // lying-fsync-truncated WAL can leave the memtable holding an OLDER version
        // than a durably-installed SSTable for the same key. So merge BY SEQ — the
        // highest qualifying seq wins; a tombstone at that seq ⇒ ∅.
        Seq best_seq = kNoSeq;
        bool best_tomb = false;
        bool best_vlog = false;
        Value best_val;
        bool found = false;

        const Memtable::Hit mh = mem_.lookup_hit(key, at);
        if (mh.covered && (!found || mh.seq >= best_seq)) {
            best_seq = mh.seq;
            best_tomb = mh.tombstone;
            best_vlog = mh.vlog;
            best_val = mh.value;
            found = true;
        }
        for (const std::unique_ptr<SSTableReader>& sst : sstables_) {
            const SSTableReader::Hit h = sst->lookup(key, at);
            if (h.covered && (!found || h.seq >= best_seq)) {
                best_seq = h.seq;
                best_tomb = h.tombstone;
                best_vlog = h.vlog;
                best_val = h.value;
                found = true;
            }
        }
        if (!found || best_tomb) {
            p.set_value(std::nullopt);
            co_return;
        }
        if (best_vlog) {
            // Deref the pointer against its vlog generation (CRC-verified). A
            // pointer that fails to resolve at READ time means the value was lost —
            // but at read time (post-recovery) a resolvable prefix has already been
            // established, so a live pointer always resolves. We still never
            // fabricate: a failed deref returns ∅ rather than garbage (V-NOTORN).
            const std::optional<Value> v = co_await deref_value(best_val);
            p.set_value(v);
            co_return;
        }
        p.set_value(std::optional<Value>(best_val));
        co_return;
    }

    [[nodiscard]] Future<Snapshot> snapshot() override {
        Promise<Snapshot> p = make_promise<Snapshot>(sched_);
        Future<Snapshot> f = p.get_future();
        p.set_value(Snapshot{last_seq_});
        return f;
    }

    [[nodiscard]] Future<Error> sync() override {
        // The durability barrier (V-DUR). WiscKey: a large-value commit wrote the
        // value to the vlog AND a pointer to the WAL — BOTH must be made durable by
        // the same barrier, so sync the active vlog FIRST (value before pointer in
        // durability order), then the WAL. After ok, every committed record + its
        // referenced value survives a crash. A lying fsync keeps a tail un-durable
        // (lost on crash, never fabricated — V-PREFIX); recovery's deref catches a
        // pointer whose vlog tail was dropped and stops the prefix there.
        if (!value_log_active()) {
            return disk_->sync();
        }
        Promise<Error> p = make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        sched_->spawn(sync_task(std::move(p)));
        return f;
    }

    core::Task sync_task(Promise<Error> p) {
        IDisk& vdisk = factory_->disk_for(vlog_gen_);
        const Error ve = co_await vdisk.sync();
        const Error we = co_await disk_->sync();
        // Surface the first error (WAL durability is the headline barrier). Both
        // being ok is the common case; a lying/io fault on either is reported.
        p.set_value(ve.ok() ? we : ve);
        co_return;
    }

    // WiscKey is active iff a positive threshold AND an LSM factory are present.
    [[nodiscard]] bool value_log_active() const noexcept {
        return value_threshold_ > 0 && factory_ != nullptr;
    }

    // Dereference a vlog-pointer (the encoded VlogPtr held as `ptr_bytes`) against
    // its generation's vlog disk. Reads ONLY the pointed-at record window and CRC-
    // verifies it; returns ∅ (never garbage) on a bad pointer / torn / missing
    // region (V-NOTORN). Async: reads via IDisk. Holds no container ref across the
    // await — `ptr_bytes` + the read buffer are owned locals (V-RKV1).
    core::Future<std::optional<Value>> deref_value(Value ptr_bytes) {
        Promise<std::optional<Value>> p = make_promise<std::optional<Value>>(sched_);
        Future<std::optional<Value>> f = p.get_future();
        sched_->spawn(deref_task(std::move(p), std::move(ptr_bytes)));
        return f;
    }

    // True iff EVERY vlog-pointer entry in `reader` resolves (CRC-verified) against
    // its vlog generation. Used at recovery to reject an SSTable whose values did
    // not durably survive (a dangling pointer ⇒ not a valid prefix). Async (reads).
    core::Future<bool> sstable_vlog_pointers_resolve(const SSTableReader& reader) {
        Promise<bool> p = make_promise<bool>(sched_);
        Future<bool> f = p.get_future();
        std::vector<SstEntry> entries;
        reader.collect_entries(entries);
        sched_->spawn(verify_vlog_task(std::move(p), std::move(entries)));
        return f;
    }

    core::Task verify_vlog_task(Promise<bool> p, std::vector<SstEntry> entries) {
        for (const SstEntry& e : entries) {
            if (!e.vlog) {
                continue;
            }
            const Value ptr_bytes = e.value;  // owned copy (V-RKV1)
            const std::optional<Value> v = co_await deref_value(ptr_bytes);
            if (!v.has_value()) {
                p.set_value(false);
                co_return;
            }
        }
        p.set_value(true);
        co_return;
    }

    core::Task deref_task(Promise<std::optional<Value>> p, Value ptr_bytes) {
        VlogPtr ptr;
        if (factory_ == nullptr || !VlogPtr::decode(ptr_bytes, ptr) ||
            ptr.value_off < kVlogRecordHeaderBytes) {
            p.set_value(std::nullopt);
            co_return;
        }
        // The record spans [value_off - header, value_off + vlen + crc). Read just
        // that window from the pointer's generation disk and verify locally.
        const std::uint64_t rec_off = ptr.value_off - kVlogRecordHeaderBytes;
        const std::size_t rec_len =
            kVlogRecordHeaderBytes + static_cast<std::size_t>(ptr.vlen) + kVlogCrcBytes;
        std::vector<std::byte> buf(rec_len);
        IDisk& vdisk = factory_->disk_for(ptr.gen);
        // A TRANSIENT read io-fault (the sim's per-op coin flip) does NOT mean the
        // value is absent — the durable bytes are there; the read just faulted.
        // RETRY (bounded) so a transient fault never fabricates an absence. A
        // NotFound (region past the durable end) or Corruption (bit-rot) is NOT
        // transient → return ∅ (never a fabricated value, V-NOTORN). The bound is
        // generous; with a finite fault probability a retry resolves deterministically.
        Error re{};
        for (int attempt = 0; attempt < kVlogReadRetries; ++attempt) {
            re = co_await vdisk.read(rec_off, std::span<std::byte>(buf.data(), buf.size()));
            if (re.ok() || re.code != ErrorCode::IoFault) {
                break;  // success, or a non-transient error — stop retrying.
            }
        }
        if (!re.ok()) {
            p.set_value(std::nullopt);  // past-end / corruption / persistent fault — never fabricate.
            co_return;
        }
        // Re-base the pointer to offset 0 within the read window and verify.
        VlogPtr local = ptr;
        local.value_off = kVlogRecordHeaderBytes;
        p.set_value(vlog_deref(buf, local));
        co_return;
    }

    [[nodiscard]] Future<std::vector<KeyValue>> scan(Range range, Snapshot snap) override {
        // LSM range read (§1/§5): merge the memtable + every SSTable at the
        // snapshot, newest-version-per-key, dropping tombstones, key-ascending.
        // WiscKey: a surviving entry that is a vlog-pointer is DEREF'd (async), so
        // scan is a coroutine on the scheduler.
        Promise<std::vector<KeyValue>> p = make_promise<std::vector<KeyValue>>(sched_);
        Future<std::vector<KeyValue>> f = p.get_future();
        sched_->spawn(scan_task(std::move(p), std::move(range), snap.at));
        return f;
    }

    core::Task scan_task(Promise<std::vector<KeyValue>> p, Range range, Seq at) {
        // Collect, per key, the NEWEST (seq, value, tombstone, vlog) with seq <= at,
        // across the memtable and all SSTables, keeping the highest seq per key.
        struct Best {
            Seq seq = kNoSeq;
            Value value;
            bool tombstone = false;
            bool vlog = false;
            bool present = false;
        };
        std::vector<std::pair<Key, Best>> merged;  // sorted-by-Key (deterministic)
        // `v` is taken BY VALUE so the caller can std::move its (owned) value in and we
        // move it onward into `merged` — one fewer value copy per offered key vs taking a
        // const ref and copying into Best. (We must NOT hold a pointer into the memtable
        // here: scan_task co_awaits in the materialise loop and a concurrent insert can
        // realloc the memtable — V-RKV1 — so the merge holds owned copies, just move-only.)
        auto offer = [&](const Key& k, Seq s, Value v, bool tomb, bool vl) {
            // Keep `merged` key-ascending and find the slot by BINARY search, not a
            // linear walk. The memtable feeds keys already ascending, so the old
            // `while (merged[i].first < k) ++i` walked the ENTIRE vector on every
            // offer → O(N) per key → O(N^2) to build a range scan (the dominant cost
            // a SQL full-scan profile surfaced: ~7us/row, growing with N). lower_bound
            // lands on the SAME slot the linear walk computed, so the insert position,
            // the key-equality branch, and the seq-tiebreak (>=) are byte-identical;
            // this is purely the search made logarithmic (the same fix find() got).
            // Fast path: an ascending feed appends at the end in O(1).
            if (merged.empty() || merged.back().first < k) {
                merged.emplace_back(k, Best{s, std::move(v), tomb, vl, true});
                return;
            }
            auto it = std::lower_bound(
                merged.begin(), merged.end(), k,
                [](const std::pair<Key, Best>& e, const Key& key) { return e.first < key; });
            if (it != merged.end() && it->first == k) {
                if (s >= it->second.seq) {
                    it->second = Best{s, std::move(v), tomb, vl, true};
                }
                return;
            }
            merged.insert(it, std::pair<Key, Best>{k, Best{s, std::move(v), tomb, vl, true}});
        };

        // SEEK to range.lo by binary search instead of walking the WHOLE memtable: keys()
        // is sorted, so a scan whose range is a small slice (a columnar column family, a
        // PK sub-range, an index lookup) touches O(slice) keys, not O(memtable). A full-
        // table scan still starts at begin (lower_bound of the table prefix) and runs to
        // the namespace end — same work as before. Stop at the first key >= range.hi.
        const std::map<Key, Memtable::Versions>& mk = mem_.entries();
        for (auto it = mk.lower_bound(range.lo); it != mk.end(); ++it) {
            if (!range.hi_unbounded && !(it->first < range.hi)) {
                break;  // past the range upper bound (keys ascending)
            }
            Memtable::Hit h = Memtable::hit_from(it->second, at);
            if (h.covered) {
                offer(it->first, h.seq, std::move(h.value), h.tombstone, h.vlog);
            }
        }
        for (const std::unique_ptr<SSTableReader>& sst : sstables_) {
            std::vector<std::pair<Key, SSTableReader::ScanCand>> acc;
            const Key hi = range.hi_unbounded ? Key{} : range.hi;
            sst->scan_into(range.lo, hi, at, range.hi_unbounded, acc);
            for (auto& c : acc) {
                offer(c.first, c.second.seq, std::move(c.second.value), c.second.tombstone,
                      c.second.vlog);
            }
        }

        // Materialise the surviving live values (deref vlog pointers). We DON'T hold
        // a reference into `merged` across the await — copy the pointer bytes out
        // first (V-RKV1); `merged` is not mutated during the deref loop anyway.
        std::vector<KeyValue> out;
        for (std::size_t i = 0; i < merged.size(); ++i) {
            if (!merged[i].second.present || merged[i].second.tombstone) {
                continue;
            }
            if (merged[i].second.vlog) {
                const Value ptr_bytes = merged[i].second.value;  // owned copy
                const Key k = merged[i].first;                   // owned copy
                const std::optional<Value> v = co_await deref_value(ptr_bytes);
                if (v.has_value()) {
                    out.emplace_back(k, *v);
                }
            } else {
                // `merged` is not read again after this materialise loop — move its
                // key + value out instead of copying (one fewer string alloc/copy per row).
                out.emplace_back(std::move(merged[i].first), std::move(merged[i].second.value));
            }
        }
        p.set_value(std::move(out));
        co_return;
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

    // Force an immediate compaction: k-way merge ALL live SSTables into ONE,
    // applying version GC under the read watermark, atomically install the merged
    // SSTable, obsolete the inputs, and raise the WAL-truncation watermark (test +
    // metamorphic hook; sim-only). A no-op with < 2 SSTables. Crash-safe.
    [[nodiscard]] Future<Error> force_compact() {
        Promise<Error> p = make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        sched_->spawn(force_compact_task(std::move(p)));
        return f;
    }

    [[nodiscard]] Seq last_seq() const noexcept { return last_seq_; }
    [[nodiscard]] std::size_t sstable_count() const noexcept { return sstables_.size(); }

    // The WAL-truncation watermark: WAL records with seq <= this are covered by
    // the durable SSTable set and skipped on recovery (their prefix is reclaimable).
    [[nodiscard]] Seq wal_trunc_seq() const noexcept { return wal_trunc_seq_; }

    // Total decoded version count across every live SSTable + the memtable — the
    // GC-space metric the GC-safety test measures before/after a compaction.
    [[nodiscard]] std::size_t live_version_count() const noexcept {
        std::size_t n = mem_.version_count();
        for (const std::unique_ptr<SSTableReader>& sst : sstables_) {
            n += sst->entry_count();
        }
        return n;
    }
    // Reclaimed SSTable backing ids (obsoleted by compaction) — the disk-GC proof.
    [[nodiscard]] const std::vector<std::uint64_t>& obsoleted_ids() const noexcept {
        return obsoleted_ids_;
    }

    // PITR SUPPORT (point-in-time recovery archive): export the committed logical
    // op-log with seq >= `from_seq` into `out`, in strict Seq order, with WiscKey
    // vlog pointers DEREFED to inline values (so the archive is self-contained /
    // portable, exactly like a logical backup). READ-ONLY: touches no durable byte
    // and mutates no engine state (the durability core is untouched).
    //
    // The op-log source is the in-memory memtable, which holds EVERY committed
    // version until it is flushed to an SSTable. A flush ERASES the flushed
    // versions from the memtable (even before WAL truncation), so once any flush
    // has happened the memtable is no longer the whole op-log. export_ops DETECTS
    // that as a Seq GAP and REFUSES rather than emit a partial log with a hole:
    // the exported seqs MUST be exactly the contiguous run [max(from_seq,1) ..
    // last_seq()] (V-PITR-COMPLETE). For the in-memory-default engine (no flush)
    // the full history is resident, so this always holds. A caller that has
    // flushed must archive BEFORE the flush (continuous archiving) or take a base
    // backup instead — the refusal is honest, never a silent gap.
    [[nodiscard]] Future<Error> export_ops(Seq from_seq, std::vector<ExportedOp>& out) {
        Promise<Error> p = make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        sched_->spawn(export_ops_task(std::move(p), from_seq, &out));
        return f;
    }

private:
    // Gather every memtable version with seq >= from_seq, sort by Seq, verify the
    // run is a gap-free [max(from_seq,1) .. last_seq_] (else REFUSE — a flush ate a
    // prefix), then deref vlog pointers to inline values. On success `*out` holds
    // the ops in ascending Seq order; on failure `*out` is cleared and the Error is
    // returned. NO memtable reference is held across the deref await (V-RKV1): the
    // versions are copied into `ops` first, then derefed.
    core::Task export_ops_task(Promise<Error> p, Seq from_seq, std::vector<ExportedOp>* out) {
        out->clear();
        std::vector<ExportedOp> ops;
        for (const auto& [key, versions] : mem_.entries()) {
            for (const Memtable::Version& v : versions) {
                if (v.seq < from_seq) {
                    continue;
                }
                ExportedOp op;
                op.seq = v.seq;
                op.key = key;
                op.value = v.value;  // may be a 16-byte vlog pointer (derefed below)
                op.tombstone = v.tombstone;
                op.vlog = v.vlog;
                ops.push_back(std::move(op));
            }
        }
        std::sort(ops.begin(), ops.end(),
                  [](const ExportedOp& a, const ExportedOp& b) { return a.seq < b.seq; });
        // Completeness: the exported seqs must be exactly the contiguous committed
        // run [s0 .. last_seq_]. Each commit gets a unique Seq (V-MONO), so a matching
        // front/back/count is sufficient to prove no gap and no duplicate.
        const Seq s0 = from_seq < 1 ? 1 : from_seq;
        if (last_seq_ >= s0) {
            const std::uint64_t want = static_cast<std::uint64_t>(last_seq_ - s0 + 1);
            if (ops.empty() || ops.front().seq != s0 || ops.back().seq != last_seq_ ||
                static_cast<std::uint64_t>(ops.size()) != want) {
                p.set_value(Error{ErrorCode::Corruption,
                                  "pitr: op-log incomplete (flushed/truncated) — archive before flush"});
                co_return;
            }
        } else {
            ops.clear();  // from_seq is beyond the committed tip — nothing to archive.
        }
        for (ExportedOp& op : ops) {
            if (op.vlog) {
                const std::optional<Value> v = co_await deref_value(op.value);
                if (!v.has_value()) {
                    p.set_value(Error{ErrorCode::Corruption,
                                      "pitr: vlog value did not resolve (dangling pointer)"});
                    co_return;
                }
                op.value = *v;
                op.vlog = false;
            }
        }
        *out = std::move(ops);
        p.set_value(Error{});
        co_return;
    }

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
        // WiscKey LARGE-VALUE SEPARATION (C3.6): if value-log is enabled and this
        // is a put of a value STRICTLY LONGER than the threshold, append the value
        // to the active vlog generation FIRST (so its durability shares this
        // commit's sync barrier — V-DUR), then store only the 16/24-byte pointer in
        // the WAL record + memtable. The vlog append precedes the WAL append, so on
        // the durable image the value bytes sit before the pointer that references
        // them; recovery derefs the pointer and drops any commit whose vlog region
        // did not survive (V-NOTORN). A small value or a tombstone stays inline.
        bool is_vlog = false;
        Value stored = value;  // what the LSM stores: inline value OR the pointer
        if (!tombstone && value_log_active() && value.size() > value_threshold_) {
            IDisk& vdisk = factory_->disk_for(vlog_gen_);
            const VlogEncoded enc =
                encode_vlog_record(value, vlog_len_, vlog_gen_);
            Offset voff = 0;
            const Error ve = co_await vdisk.append(
                std::span<const std::byte>(enc.bytes.data(), enc.bytes.size()), voff);
            if (ve.ok()) {
                // The append landed (possibly torn — recovery's CRC catches a torn
                // tail; the in-memory pointer below still resolves pre-crash). Track
                // the logical vlog length so the next record's base_off is correct.
                vlog_len_ += enc.bytes.size();
                stored = enc.ptr.encode();
                is_vlog = true;
            }
            // On an io-fault the vlog append had NO effect (no bytes); fall back to
            // storing the value INLINE so the commit is still correct + durable.
        }

        WalRecord rec;
        rec.seq = seq;
        rec.tombstone = tombstone;
        rec.vlog = is_vlog;
        rec.key = key;            // keep a copy for the post-await memtable insert
        rec.value = stored;
        const std::vector<std::byte> bytes = encode_record(rec);

        Offset off = 0;
        // Await the disk append. The Error is the co_await value (set_value path).
        const Error e =
            co_await disk_->append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
        (void)e;  // a torn/io-faulted append is fine: recovery's CRC catches it;
                  // the in-memory memtable still reflects the (pre-durability)
                  // commit, and a crash before sync drops it cleanly (V-PREFIX).

        // Insert AFTER the await — we held no memtable reference across it.
        mem_.insert(key, seq, std::move(stored), tombstone, is_vlog);

        // Flush BEFORE returning the commit Seq if the memtable is over the
        // threshold AND we are in LSM mode. Flush is crash-safe (atomic-install
        // via the manifest); doing it inline here keeps the engine single-flow
        // (no background thread; everything on the scheduler). The commit's Seq is
        // already assigned + WAL-durable-on-next-sync, so flushing now never loses
        // it. A flush failure (io fault) leaves the memtable intact — the versions
        // remain readable from memory + WAL; the next flush retries.
        if (flush_threshold_ > 0 && mem_.flushable_version_count() > flush_threshold_) {
            co_await do_flush();
            // SIZE-TIERED COMPACTION (C3.4): once the live SSTable count reaches
            // the trigger, merge them all into one (deterministic, inline on the
            // scheduler — no background thread). Crash-safe (atomic install +
            // obsolete via the manifest). A failure leaves the live set intact.
            if (compaction_trigger_ >= 2 && sstables_.size() >= compaction_trigger_) {
                co_await do_compact();
            }
        }
        p.set_value(seq);
        co_return;
    }

    core::Task force_flush_task(Promise<Error> p) {
        co_await do_flush();
        p.set_value(Error{});
        co_return;
    }

    core::Task force_compact_task(Promise<Error> p) {
        co_await do_compact();
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
        // WiscKey: the memtable versions we are about to flush may carry vlog
        // pointers into the ACTIVE write generation whose value bytes are still
        // staged/lying. The SSTable we install must NOT reference a non-durable
        // vlog region (else a crash leaves a DANGLING pointer that reads ∅ where a
        // committed value should be — a prefix violation). So make the active vlog
        // DURABLE before the SSTable is installed (value before pointer, V-NOTORN).
        // A lying/io fault here aborts the flush (the data stays in memtable + WAL).
        if (value_log_active() && vlog_len_ > 0) {
            IDisk& vdisk = factory_->disk_for(vlog_gen_);
            const Error vse = co_await vdisk.sync();
            if (!vse.ok()) {
                co_return;
            }
        }
        // (1) snapshot the memtable → entries (key-asc, seq-asc — its native order).
        // SELECTIVE FLUSH: skip keep-resident namespaces (the columnar engine manages
        // those; flushing them only churns). With no resident byte set, every key is
        // taken (the original whole-memtable snapshot, byte-identical).
        std::vector<SstEntry> entries;
        for (const auto& [k, versions] : mem_.entries()) {
            if (mem_.is_resident(k)) {
                continue;
            }
            for (const Memtable::Version& v : versions) {
                // Carry the WiscKey vlog flag so a separated value's POINTER (not
                // the value) is what flushes to the SSTable (no value rewrite).
                entries.push_back(SstEntry{k, v.value, v.seq, v.tombstone, v.vlog});
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
        // Drop only the flushed (non-resident) versions; resident (columnar) keys
        // stay. With no resident byte set this erases the whole memtable (== clear()).
        mem_.erase_flushable();
        co_return;
    }

    // ---- COMPACTION (C3.4) + VERSION GC (V-GC) + WAL TRUNCATION ----------------
    //
    // Merge ALL live SSTables into ONE, dropping versions no live snapshot can see,
    // then truncate the WAL prefix the merged set now durably covers. CRASH-SAFE
    // via the append-only manifest, reusing the step-3 atomic-install discipline at
    // a coarser grain:
    //   (1) snapshot the live SSTable set (ids + entries) + the max covered seq.
    //   (2) k-way merge their entries + GC under the read watermark → merged run.
    //   (3) build the merged SSTable image, append to a NEW disk, SYNC it.
    //   (4) append a CRC'd INSTALL manifest record (new id) + SYNC → the merged
    //       SSTable is now LIVE. A crash before this: the merged bytes are inert,
    //       the OLD set is still installed → reads unchanged (no loss).
    //   (5) append a CRC'd OBSOLETE record per OLD id + SYNC → the inputs are now
    //       dead (recovery will not load them). A crash between (4) and (5): BOTH
    //       old + new load; the read path merges by max-seq → identical values, no
    //       fabrication, no loss (just transient redundancy, healed next compaction).
    //   (6) append a CRC'd WAL-TRUNCATE record at the contiguous prefix the merged
    //       SSTable durably covers + SYNC → recovery may skip that WAL prefix. A
    //       crash before this: the WAL is replayed in full (harmless duplicates).
    //   (7) swap the in-memory live set (drop obsoleted, add merged), reclaim the
    //       old disks, raise wal_trunc_seq_. NEVER a mix that loses/fabricates.
    core::Task do_compact() {
        if (manifest_disk_ == nullptr || factory_ == nullptr || sstables_.size() < 2) {
            co_return;
        }
        // (1) snapshot the live inputs. Entries (key-asc/seq-asc within each run),
        // ids (to obsolete), and the max seq covered (the truncation candidate).
        std::vector<std::vector<SstEntry>> runs;
        std::vector<std::uint64_t> input_ids;
        Seq covered_max = kNoSeq;
        for (const std::unique_ptr<SSTableReader>& sst : sstables_) {
            std::vector<SstEntry> run;
            sst->collect_entries(run);
            runs.push_back(std::move(run));
            input_ids.push_back(sst->sstable_id);
            if (sst->max_seq > covered_max) {
                covered_max = sst->max_seq;
            }
        }

        // (2) k-way merge + version GC under the watermark. The watermark is the
        // oldest live snapshot Seq; below-shadowed versions are dropped (V-GC).
        std::vector<SstEntry> merged = compact_merge(runs, read_watermark_);

        // (2b) WiscKey VLOG GC (V-GC for values): the version GC above already
        // dropped dead LSM entries; their large values in the OLD vlog generations
        // are now garbage. Rewrite the SURVIVING vlog-pointer values into a FRESH
        // vlog generation and re-point the merged entries at it. After the merged
        // SSTable + this fresh vlog are durable and the inputs obsoleted, the OLD
        // input vlog generations hold ONLY dead values → reclaimable. (Conservative
        // + correct: we never drop a generation the active WRITE log or a surviving
        // SSTable/WAL prefix still references — see reclaim filter below.)
        std::vector<std::uint64_t> reclaim_gens;
        if (value_log_active()) {
            co_await rewrite_vlog_for_compaction(merged, reclaim_gens);
        }

        if (merged.empty()) {
            // GC dropped everything (e.g. a single tombstone fully below the
            // watermark). We still must obsolete the inputs to reclaim them, but
            // there is no merged SSTable to install. Handle by obsoleting only.
            co_await obsolete_and_truncate(input_ids, /*new_id_present=*/false,
                                           /*new_id=*/0, /*new_reader=*/nullptr,
                                           covered_max, reclaim_gens);
            co_return;
        }

        // (3) build + durably write the merged SSTable.
        const SstBuildResult built = SSTableBuilder::build(merged);
        const std::uint64_t new_id = next_sstable_id_++;
        IDisk& ndisk = factory_->disk_for(new_id);
        Offset off = 0;
        const Error ae = co_await ndisk.append(
            std::span<const std::byte>(built.bytes.data(), built.bytes.size()), off);
        if (!ae.ok()) {
            co_return;  // merged image did not land — abort, live set untouched.
        }
        const Error se = co_await ndisk.sync();
        if (!se.ok()) {
            co_return;
        }

        // (4) INSTALL the merged SSTable (atomic — live only after this sync).
        ManifestRecord mr;
        mr.kind = ManifestKind::Install;
        mr.entry_no = ++manifest_tip_;
        mr.sstable_id = new_id;
        mr.sst_len = built.bytes.size();
        mr.min_seq = built.min_seq;
        mr.max_seq = built.max_seq;
        const std::vector<std::byte> mbytes = encode_manifest(mr);
        Offset moff = 0;
        const Error me = co_await manifest_disk_->append(
            std::span<const std::byte>(mbytes.data(), mbytes.size()), moff);
        if (!me.ok()) {
            --manifest_tip_;
            co_return;
        }
        const Error mse = co_await manifest_disk_->sync();
        if (!mse.ok()) {
            --manifest_tip_;
            co_return;
        }

        // Build the in-memory reader for the merged SSTable (parsed via the same
        // validated path as recovery) so it can be installed once obsoletes land.
        auto new_reader = std::make_unique<SSTableReader>();
        if (!SSTableLoader::parse(built.bytes, new_id, *new_reader)) {
            // Should never happen (we just built it); be safe — keep old set live.
            co_return;
        }

        // (5)+(6) obsolete the inputs + raise the WAL-truncation watermark.
        co_await obsolete_and_truncate(input_ids, /*new_id_present=*/true, new_id,
                                       std::move(new_reader), covered_max, reclaim_gens);
        co_return;
    }

    // ---- WiscKey VLOG GC: rewrite live values into a fresh generation -----------
    //
    // For every SURVIVING entry in `merged` that is a vlog-pointer, read its value
    // from the OLD generation, append it to a FRESH vlog generation, and re-point
    // the entry at the new generation. Sync the fresh vlog so its values are durable
    // BEFORE the merged SSTable that references them is installed (durability order:
    // value before pointer, V-NOTORN). The set of OLD input generations no longer
    // referenced by any SURVIVING source becomes reclaimable (returned in
    // `reclaim_gens`); the active WRITE generation is never reclaimed (the memtable
    // tail + un-truncated WAL prefix may still point into it). A read fault on an
    // old value is conservative: we KEEP the old pointer (do not rewrite) so the
    // value is never dropped — that generation then stays referenced + is NOT
    // reclaimed (no silent loss).
    core::Task rewrite_vlog_for_compaction(std::vector<SstEntry>& merged,
                                           std::vector<std::uint64_t>& reclaim_gens) {
        // Gather the OLD generations referenced by the surviving entries.
        std::vector<std::uint64_t> old_gens;
        bool any_vlog = false;
        for (const SstEntry& e : merged) {
            if (!e.vlog) {
                continue;
            }
            any_vlog = true;
            VlogPtr ptr;
            if (VlogPtr::decode(e.value, ptr)) {
                bool seen = false;
                for (std::uint64_t g : old_gens) {
                    if (g == ptr.gen) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    old_gens.push_back(ptr.gen);
                }
            }
        }
        if (!any_vlog) {
            co_return;  // nothing separated — pure-inline compaction.
        }

        // Allocate a fresh generation for the rewritten live values.
        const std::uint64_t fresh_gen = vlog_base_id_ + (++vlog_gen_counter_);
        IDisk& fresh = factory_->disk_for(fresh_gen);
        std::uint64_t fresh_len = 0;
        bool kept_an_old_gen = false;  // a deref failure forced us to keep an old ptr

        for (SstEntry& e : merged) {
            if (!e.vlog) {
                continue;
            }
            // Deref the old value (owned copy of the pointer bytes — V-RKV1).
            const Value old_ptr_bytes = e.value;
            const std::optional<Value> v = co_await deref_value(old_ptr_bytes);
            if (!v.has_value()) {
                // Could not read the old value (io-fault). KEEP the old pointer so
                // the value is never lost; its generation stays referenced.
                kept_an_old_gen = true;
                continue;
            }
            // Append the value to the fresh generation; re-point the entry.
            const VlogEncoded enc = encode_vlog_record(*v, fresh_len, fresh_gen);
            Offset voff = 0;
            const Error ae = co_await fresh.append(
                std::span<const std::byte>(enc.bytes.data(), enc.bytes.size()), voff);
            if (!ae.ok()) {
                kept_an_old_gen = true;  // keep old pointer; do not lose the value.
                continue;
            }
            fresh_len += enc.bytes.size();
            e.value = enc.ptr.encode();
        }
        // Make the fresh vlog durable BEFORE the merged SSTable install references it.
        const Error fse = co_await fresh.sync();
        if (!fse.ok()) {
            // The fresh vlog did not durably land. We cannot safely re-point; the
            // merged entries we already re-pointed would dangle. Abort the rewrite:
            // restore is impossible here, so conservatively reclaim NOTHING and let
            // the next compaction retry. (The merged entries still point at the
            // fresh gen in memory; but since we reclaim nothing AND the SSTable
            // install is yet to come, a crash drops the half state — old set stays.)
            co_return;
        }

        // Old input generations are reclaimable iff NOT the active write generation
        // (the memtable tail / un-truncated WAL may still reference it) AND we did
        // not have to keep an old pointer for them. Conservative: if any deref/append
        // failed we reclaim nothing this round (a later compaction retries).
        if (!kept_an_old_gen) {
            for (std::uint64_t g : old_gens) {
                if (g != vlog_gen_) {
                    reclaim_gens.push_back(g);
                }
            }
        }
        co_return;
    }

    // Append OBSOLETE records for each input id, then a WAL-TRUNCATE record, each
    // CRC'd + entry_no-contiguous + synced. On success swap the in-memory live set
    // (drop obsoleted readers, add the merged reader if present), reclaim the old
    // disks (+ reclaimable vlog generations), and raise wal_trunc_seq_. A failure
    // mid-way leaves a valid recoverable prefix (the manifest fold tolerates a
    // partial obsolete suffix — see recovery).
    core::Task obsolete_and_truncate(std::vector<std::uint64_t> input_ids,
                                     bool new_id_present, std::uint64_t new_id,
                                     std::unique_ptr<SSTableReader> new_reader,
                                     Seq covered_max,
                                     std::vector<std::uint64_t> reclaim_gens) {
        for (std::uint64_t oid : input_ids) {
            const std::vector<std::byte> ob =
                encode_manifest_obsolete(++manifest_tip_, oid);
            Offset ooff = 0;
            const Error oe = co_await manifest_disk_->append(
                std::span<const std::byte>(ob.data(), ob.size()), ooff);
            if (!oe.ok()) {
                --manifest_tip_;
                co_return;  // a partial obsolete suffix — recovery heals it.
            }
            const Error ose = co_await manifest_disk_->sync();
            if (!ose.ok()) {
                --manifest_tip_;
                co_return;
            }
        }

        // WAL-TRUNCATE: the merged set durably covers seqs up to covered_max. We
        // only advance the watermark over the CONTIGUOUS committed prefix that the
        // SURVIVING SSTable set fully covers, so a record below it is genuinely
        // reproducible from SSTables. covered_max is the max seq of the inputs we
        // just merged; the merged output covers the same [.., covered_max] (GC
        // never drops the newest-visible version). We never lower the watermark.
        //
        // SELECTIVE FLUSH: when keep-resident namespaces are active, WAL truncation is
        // DISABLED. Resident keys are NEVER written to an SSTable, so a truncation
        // watermark advanced past their WAL records would make recovery skip them and
        // lose committed data. Keeping the full WAL (never truncating) lets recovery
        // replay the resident keys back. The trade is an un-truncated (in-memory)
        // WAL — acceptable for the in-memory default backing where recovery is rare.
        if (covered_max > wal_trunc_seq_ && !mem_.any_resident()) {
            const std::vector<std::byte> tr =
                encode_manifest_wal_trunc(++manifest_tip_, covered_max);
            Offset toff = 0;
            const Error te = co_await manifest_disk_->append(
                std::span<const std::byte>(tr.data(), tr.size()), toff);
            if (!te.ok()) {
                --manifest_tip_;
                co_return;
            }
            const Error tse = co_await manifest_disk_->sync();
            if (!tse.ok()) {
                --manifest_tip_;
                co_return;
            }
            wal_trunc_seq_ = covered_max;
        }

        // (7) swap the in-memory live set. Remove obsoleted readers, add the merged
        // reader (if any). Reclaim the obsoleted backing disks.
        std::vector<std::unique_ptr<SSTableReader>> kept;
        for (std::unique_ptr<SSTableReader>& sst : sstables_) {
            bool obsolete = false;
            for (std::uint64_t oid : input_ids) {
                if (sst->sstable_id == oid) {
                    obsolete = true;
                    break;
                }
            }
            if (obsolete) {
                obsoleted_ids_.push_back(sst->sstable_id);
                factory_->reclaim(sst->sstable_id);  // free the obsolete disk
            } else {
                kept.push_back(std::move(sst));
            }
        }
        if (new_id_present && new_reader != nullptr) {
            kept.push_back(std::move(new_reader));
        }
        sstables_ = std::move(kept);
        (void)new_id;

        // WiscKey VLOG GC: reclaim the OLD vlog generations now superseded by the
        // fresh rewrite. Safe ONLY here — after the merged SSTable INSTALL, the
        // input OBSOLETEs, AND the WAL-TRUNCATE are all durable: no surviving
        // SSTable, no un-truncated WAL prefix, and not the active write log
        // references them, so they hold only dead values. A crash BEFORE this
        // point leaves them intact (recovery re-reads them via the old pointers).
        for (std::uint64_t g : reclaim_gens) {
            reclaimed_vlog_ids_.push_back(g);
            factory_->reclaim(g);  // free the dead vlog generation's bytes.
        }
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

        wal_trunc_seq_ = kNoSeq;
        obsoleted_ids_.clear();

        // WiscKey: start the post-recovery WRITE generation FRESH (past every used
        // generation) so new appends never land inside a partially-durable old vlog
        // (offset confusion). Old generations stay on their disks + readable via the
        // self-identifying pointer.gen for already-committed pointers. The recovered
        // active write log begins empty (vlog_len_ = 0).
        if (value_log_active()) {
            vlog_gen_ = vlog_base_id_ + (++vlog_gen_counter_);
            vlog_len_ = 0;
        }

        // (0) LSM: FOLD the append-only manifest to compute the LIVE SSTable set +
        // the WAL-truncation watermark, THEN load the survivors. The manifest mixes
        // INSTALL / OBSOLETE / WAL-TRUNCATE records, one shared entry_no sequence.
        // Stop-at-first-corrupt AND stop-at-gap on entry_no (Seq-contiguity): a
        // torn manifest tail can never install/obsolete past a missing earlier
        // record. We fold in entry_no ORDER so an OBSOLETE after its INSTALL
        // correctly removes it (a crash between INSTALL and OBSOLETE leaves BOTH
        // the old + the merged SSTable live — the read path merges by max-seq, so
        // values are identical, no loss, no fabrication).
        struct LiveRec {
            std::uint64_t sstable_id = 0;
            std::uint64_t sst_len = 0;
            Seq max_seq = kNoSeq;
        };
        std::vector<LiveRec> live;  // live INSTALLs in install order (old→new)
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
                        break;  // torn/corrupt manifest tail → fold-prefix end.
                    }
                    if (md.record.entry_no != expect_entry) {
                        break;  // a gap in entry order → stop (Seq-contiguity).
                    }
                    switch (md.record.kind) {
                        case ManifestKind::Install:
                            live.push_back(LiveRec{md.record.sstable_id,
                                                   md.record.sst_len, md.record.max_seq});
                            if (md.record.sstable_id + 1 > next_sstable_id_) {
                                next_sstable_id_ = md.record.sstable_id + 1;
                            }
                            break;
                        case ManifestKind::Obsolete: {
                            // Drop the obsoleted SSTable from the live set + reclaim
                            // its disk (compaction superseded it; durably recorded).
                            std::vector<LiveRec> filtered;
                            for (const LiveRec& lr : live) {
                                if (lr.sstable_id == md.record.sstable_id) {
                                    obsoleted_ids_.push_back(lr.sstable_id);
                                    factory_->reclaim(lr.sstable_id);
                                } else {
                                    filtered.push_back(lr);
                                }
                            }
                            live = std::move(filtered);
                            break;
                        }
                        case ManifestKind::WalTrunc:
                            if (md.record.wal_trunc_seq > wal_trunc_seq_) {
                                wal_trunc_seq_ = md.record.wal_trunc_seq;
                            }
                            break;
                    }
                    manifest_tip_ = md.record.entry_no;
                    mpos += md.consumed;
                }
            }
        }

        // Load + fully validate every SURVIVING SSTable in install order. A
        // rejected SSTable (torn/corrupt) STOPS the load prefix — a later table
        // must not load past a missing earlier one (Seq-contiguity at SSTable
        // granularity); its data, if it predates the crash, is still in the WAL.
        Seq loaded_sst_max = kNoSeq;
        for (const LiveRec& lr : live) {
            IDisk& sdisk = factory_->disk_for(lr.sstable_id);
            auto reader = std::make_unique<SSTableReader>();
            Error lerr{};
            co_await SSTableLoader::load(sdisk, lr.sst_len, lr.sstable_id, *reader, lerr);
            if (!lerr.ok()) {
                break;
            }
            // WiscKey: an installed SSTable may carry vlog pointers whose value
            // bytes did NOT durably survive (a lying-fsync dropped the vlog tail
            // BEFORE the flush, or a vlog generation was lost). A pointer that does
            // not resolve means that committed version is NOT durable → the SSTable
            // is not a valid prefix. REJECT it (Seq-contiguity at SSTable grain): its
            // data, if it predates the crash, is still in the WAL prefix (replayed
            // with the SAME deref discipline). Never advance the tip past a dangling
            // pointer (that would leave a hole below the tip — a V-PREFIX violation).
            if (value_log_active() && !(co_await sstable_vlog_pointers_resolve(*reader))) {
                break;
            }
            if (reader->max_seq > last_seq_) {
                last_seq_ = reader->max_seq;
            }
            if (reader->max_seq > loaded_sst_max) {
                loaded_sst_max = reader->max_seq;
            }
            sstables_.push_back(std::move(reader));
        }
        // SAFETY CLAMP: only skip WAL records the LOADED SSTable set actually
        // covers. If an SSTable referenced by a WAL-TRUNCATE watermark failed to
        // load (torn/rejected), we must NOT skip its WAL prefix — replay it. The
        // loaded survivors cover a contiguous [1, loaded_sst_max] prefix, so clamp
        // the effective truncation to that. (Never skip a record we cannot
        // reproduce from an SSTable — that would lose a committed value.)
        if (wal_trunc_seq_ > loaded_sst_max) {
            wal_trunc_seq_ = loaded_sst_max;
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
        // WAL-TRUNCATION (C3.4): records with seq <= wal_trunc_seq_ are durably
        // covered by the loaded SSTable set, so the WAL prefix below the watermark
        // is reclaimable — recovery SKIPS replaying it (proving the SSTables alone
        // reconstruct that prefix). Contiguity still applies from the watermark up:
        // the first non-truncated record must be exactly wal_trunc_seq_+1.
        Seq expect = wal_trunc_seq_ + 1;  // first replayed Seq after the watermark.
        while (pos < image.size()) {
            const DecodeResult dr = try_decode(image, pos);
            if (!dr.ok) {
                break;  // first corrupt/torn record → consistent-prefix boundary.
            }
            // A record at/below the truncation watermark is covered by an SSTable;
            // skip replaying it (it is logically truncated). It must be contiguous
            // BELOW the watermark too (a gap in the truncated prefix would mean a
            // lost commit the SSTables claim to cover — but we clamped the watermark
            // to the loaded SSTable max, so [1, wal_trunc_seq_] is fully covered).
            if (dr.record.seq <= wal_trunc_seq_) {
                pos += dr.consumed;
                continue;
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
            // WiscKey CRASH DISCIPLINE (V-NOTORN / V-PREFIX — the crux): a record
            // that carries a vlog POINTER is part of the prefix ONLY IF its value
            // durably survived in the vlog. The vlog append+sync shares this
            // commit's barrier, so the value bytes should be durable BEFORE the
            // WAL pointer is. If the pointer does NOT resolve (vlog region torn /
            // lying-fsync-dropped / past the durable end / CRC-mismatch), the
            // commit was NOT fully durable → it is NOT part of the recovered prefix.
            // We treat it EXACTLY like a corrupt WAL record: STOP here. A torn/
            // missing vlog region can therefore never resolve to a fabricated value.
            if (dr.record.vlog) {
                const std::optional<Value> v = co_await deref_value(dr.record.value);
                if (!v.has_value()) {
                    break;  // pointer's value not durable → prefix ends here.
                }
            }
            mem_.insert(dr.record.key, dr.record.seq, dr.record.value,
                        dr.record.tombstone, dr.record.vlog);
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

    // ---- compaction (C3.4) + GC (V-GC) + WAL-truncation state -------------
    std::size_t compaction_trigger_ = 0;      // live SSTable count to compact at (0=off)
    Seq read_watermark_ = kNoSeq;             // oldest live snapshot Seq (V-GC bound)
    Seq wal_trunc_seq_ = kNoSeq;              // WAL records <= this are SSTable-covered
    std::vector<std::uint64_t> obsoleted_ids_;  // backings reclaimed by compaction

    // ---- WiscKey value-log (C3.6) state -----------------------------------
    std::size_t value_threshold_ = 0;          // value len > this ⇒ separated (0=off)
    std::uint64_t vlog_base_id_ = kDefaultVlogBaseId;  // factory id base for vlogs
    std::uint64_t vlog_gen_ = kDefaultVlogBaseId;      // active WRITE generation id
    std::uint64_t vlog_len_ = 0;               // logical len of the active vlog
    std::uint64_t vlog_gen_counter_ = 0;     // monotonic vlog generation allocator
    std::vector<std::uint64_t> reclaimed_vlog_ids_;    // vlog generations reclaimed by GC

public:
    // ---- WiscKey introspection (tests / fingerprints; never used for ordering) --
    // The active write generation id + its logical length (the active vlog metric).
    [[nodiscard]] std::uint64_t vlog_active_gen() const noexcept { return vlog_gen_; }
    [[nodiscard]] std::uint64_t vlog_active_len() const noexcept { return vlog_len_; }
    // Vlog generations reclaimed by compaction GC (the disk-GC proof for values).
    [[nodiscard]] const std::vector<std::uint64_t>& reclaimed_vlog_ids() const noexcept {
        return reclaimed_vlog_ids_;
    }
};

}  // namespace lockstep::storage
