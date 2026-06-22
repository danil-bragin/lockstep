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

#include <lockstep/storage/Engine.hpp>

namespace lockstep::storage {

using core::Error;
using core::ErrorCode;
using core::Future;
using core::IDisk;
using core::make_promise;
using core::Offset;
using core::Promise;
using core::Scheduler;

// ---------------------------------------------------------------------------
// Hand-rolled CRC32 (IEEE 802.3 polynomial 0xEDB88320, reflected). Fully
// specified + table-free-at-call (the table is built once, deterministically) so
// the integrity check is byte-identical across platforms — the same shape as the
// batch-2 per-record CRC fix. NOT a library call (storage/ is not lint-exempt and
// this must be deterministic + auditable).
// ---------------------------------------------------------------------------
class Crc32 {
public:
    [[nodiscard]] static std::uint32_t compute(std::span<const std::byte> data) noexcept {
        const std::uint32_t* tbl = table();
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::byte b : data) {
            const std::uint8_t idx =
                static_cast<std::uint8_t>(crc) ^ std::to_integer<std::uint8_t>(b);
            crc = (crc >> 8) ^ tbl[idx];
        }
        return crc ^ 0xFFFFFFFFu;
    }

private:
    // Build the 256-entry lookup table once (function-local static, single-
    // threaded deterministic init). Reflected CRC32, polynomial 0xEDB88320.
    [[nodiscard]] static const std::uint32_t* table() noexcept {
        static const std::array<std::uint32_t, 256> kTable = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                t[i] = c;
            }
            return t;
        }();
        return kTable.data();
    }
};

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

// ---- little-endian fixed-width encode helpers (deterministic, no <bit>) -----
inline void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
}
inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}
[[nodiscard]] inline std::uint32_t get_u32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24);
}
[[nodiscard]] inline std::uint64_t get_u64(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

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
        const KeyVersions* kv = find(key);
        if (kv == nullptr) {
            return std::nullopt;
        }
        const Version* newest = nullptr;
        for (const Version& v : kv->versions) {
            if (v.seq <= at) {
                newest = &v;  // ascending ⇒ later hits are strictly newer
            } else {
                break;
            }
        }
        if (newest == nullptr || newest->tombstone) {
            return std::nullopt;
        }
        return newest->value;
    }

    void clear() { keys_.clear(); }

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
// WalEngine — the concrete Engine (Engine.hpp) over WAL + memtable + MVCC.
// ---------------------------------------------------------------------------
class WalEngine final : public Engine {
public:
    WalEngine(Scheduler& sched, IDisk& disk) noexcept : sched_(&sched), disk_(&disk) {}

    [[nodiscard]] Future<Seq> put(Key key, Value value) override {
        return commit(std::move(key), std::move(value), /*tombstone=*/false);
    }

    [[nodiscard]] Future<Seq> del(Key key) override {
        return commit(std::move(key), Value{}, /*tombstone=*/true);
    }

    [[nodiscard]] Future<std::optional<Value>> get(Key key, Snapshot snap) override {
        // Pure in-memory MVCC read. The memtable holds the whole committed history
        // (no SSTable yet); resolve AFTER no pending await (we hold no reference
        // across one — there is none here). V-SNAP: never a version > snap.at.
        Promise<std::optional<Value>> p = make_promise<std::optional<Value>>(sched_);
        Future<std::optional<Value>> f = p.get_future();
        p.set_value(mem_.lookup(key, snap.at));
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
        sched_->spawn(recover_task(std::move(p), durable_len));
        return f;
    }

    [[nodiscard]] Seq last_seq() const noexcept { return last_seq_; }

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
        p.set_value(seq);
        co_return;
    }

    core::Task recover_task(Promise<Error> p, std::size_t durable_len) {
        // Read the durable image back through the disk (a real recover reads the
        // platter). The buffer is an owned local (no reference across the await).
        std::vector<std::byte> image(durable_len);
        if (durable_len > 0) {
            const Error re =
                co_await disk_->read(0, std::span<std::byte>(image.data(), image.size()));
            if (!re.ok()) {
                // A read fault / corruption covering the prefix: we cannot trust
                // any of it. Recover to EMPTY rather than fabricate (V-NOTORN).
                mem_.clear();
                last_seq_ = kNoSeq;
                p.set_value(Error{});
                co_return;
            }
        }

        // Decode + replay front-to-back; stop at the first integrity failure.
        mem_.clear();
        last_seq_ = kNoSeq;
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
            last_seq_ = dr.record.seq;  // monotonic + contiguous; track the tip
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
};

}  // namespace lockstep::storage
