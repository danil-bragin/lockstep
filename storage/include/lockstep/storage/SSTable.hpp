#pragma once

// SSTable.hpp — Phase 3 §5 step 4 (C3.3). The on-disk, IMMUTABLE sorted run of
// MVCC versions that the memtable flushes into, plus the per-SSTable BLOOM filter
// and SPARSE INDEX used to skip/seek on the read path, plus the MANIFEST that
// atomically "installs" an SSTable as live. All bytes go through core::IDisk
// (the sim provider models torn writes / lying fsync / crash); the integrity
// discipline is the batch-2 lesson reused at the block + manifest granularity:
//   * PER-BLOCK CRC32 (V-NOTORN): every block carries a trailing CRC over its
//     payload; a torn/flipped block fails the check.
//   * STOP-AT-FIRST-CORRUPT + the on-disk frame (footer magic + lengths) so a
//     truncated/torn SSTable tail is detected and the SSTable is rejected whole.
//   * ATOMIC INSTALL via the MANIFEST: an SSTable is only "live" once its record
//     is DURABLY in the manifest, and that record is itself CRC'd AND
//     Seq-contiguous (manifest entry numbers 1,2,3,…). A crash mid-flush leaves
//     either the old state (no manifest record) or a complete, integrity-checked
//     SSTable referenced by a committed manifest record — NEVER a half-SSTable
//     that recovers as valid. (A valid block CRC does NOT make a partial SSTable
//     part of the prefix if its manifest record is missing — the same shape as
//     the WAL Seq-contiguity guard.)
//
// ----------------------------------------------------------------------------
// SSTABLE ON-DISK FORMAT (little-endian; one append-structured IDisk per table):
//
//   [ DATA BLOCK 0 ][ DATA BLOCK 1 ]...[ DATA BLOCK n-1 ][ INDEX BLOCK ][ BLOOM BLOCK ][ FOOTER ]
//
//   BLOCK framing (every block — data/index/bloom):  [ payload bytes ][ u32 crc ]
//     crc = CRC32 over the payload bytes only. A read re-checks it (V-NOTORN).
//
//   DATA BLOCK payload = a run of ENTRIES, key-ascending, then Seq-ascending per
//   key (the memtable's order). One entry:
//       u32 klen | u32 vlen | u64 seq | u8 tomb | key[klen] | value[vlen]
//   Entries are packed; a block ends when adding the next entry would exceed the
//   target block size (so a block holds ≥1 entry; large entries get their own).
//
//   INDEX BLOCK payload (sparse index: one record per data block) = a run of:
//       u64 block_off | u32 block_len | u32 first_klen | first_key[first_klen]
//   block_off/len address the data block's framed bytes; first_key is the first
//   key in that block. Binary-searchable by key (block-granular seek, §3).
//
//   BLOOM BLOCK payload = a hand-rolled, deterministic Bloom filter over every
//   key in the table:  u32 nbits | u32 nhash | bits[ceil(nbits/8)] (LE bit i in
//   byte i/8, bit i%8). NEVER a false negative: a key present in the table is
//   always reported "maybe present" (§ bloom correctness). False positives fine.
//
//   FOOTER (fixed 48 bytes at end-of-file):
//       [0]  u64 index_off | [8]  u32 index_len | [12] u64 bloom_off |
//       [20] u32 bloom_len | [24] u64 min_seq   | [32] u64 max_seq   |
//       [40] u32 version(W2) | [44] u32 magic(=0x4C535354 'LSST') | [48] u32 crc
//   crc = CRC32 over the first 48 footer bytes. The reader reads the last 52
//   bytes, checks magic+crc+version, then seeks index/bloom by their framed offsets.
//
// ----------------------------------------------------------------------------
// MANIFEST ON-DISK FORMAT (its own append-structured IDisk; the atomic-install
// log). Append-only run of CRC'd, Seq-contiguous records:
//       u32 magic(=0x4C4D414E 'LMAN') | u64 entry_no | u64 sstable_id |
//       u64 sst_len | u64 min_seq | u64 max_seq | u32 crc
//   crc = CRC32 over the record up to (not incl.) the crc. entry_no is 1,2,3,…
//   (Seq-contiguity: replay stops at the first gap OR first CRC fail — a torn
//   manifest tail can never install a partial SSTable). sstable_id selects which
//   IDisk backs that SSTable (the disk factory keys on it); sst_len is the
//   durable length the reader trusts (so a torn SSTable tail beyond sst_len is
//   never read). An SSTable whose record never landed durably is INVISIBLE.
//
// ----------------------------------------------------------------------------
// READ PATH (§3): newest→oldest. The memtable (newest versions, ≤ snap) is
// consulted first; then SSTables in REVERSE install order (newest first). For
// each SSTable: the bloom filter skips it if the key is definitely absent; else
// the sparse index seeks the candidate block; the block is read + CRC-checked +
// scanned for the newest version of the key with seq ≤ snap.at. The first
// SSTable that yields a version for the key wins (newer installs shadow older).
//
// SCAN (§1/§5): scan(range, snap) merges the memtable + every SSTable at the
// snapshot — for each key in [lo,hi) the newest version with seq ≤ snap.at,
// skipping tombstones — and returns the surviving (Key,Value) pairs key-ordered.
//
// FORBIDDEN (storage/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, <random>, raw file IO, unordered iteration affecting
// output. All IO is core::IDisk; the CRC + bloom hashes are hand-rolled +
// deterministic. A pure function of (seed, ops).

#include <algorithm>
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

#include <lockstep/storage/Codec.hpp>   // Crc32, put_u32/u64, get_u32/u64
#include <lockstep/storage/Engine.hpp>  // Key, Value, Seq, kNoSeq
#include <lockstep/storage/Format.hpp>  // W2: format::kSstableVersion

namespace lockstep::storage {

using core::Error;
using core::ErrorCode;
using core::IDisk;
using core::Offset;

// ---------------------------------------------------------------------------
// The logical, decoded form of one MVCC version as stored in an SSTable. (The
// on-disk byte layout is documented at the top of this file.)
// ---------------------------------------------------------------------------
struct SstEntry {
    Key key;
    Value value;       // inline value, OR (vlog==true) the encoded VlogPtr
    Seq seq = kNoSeq;
    bool tombstone = false;
    bool vlog = false;  // WiscKey large-value separation: `value` is a VlogPtr
};

// Fixed framing constants — the on-disk contract.
inline constexpr std::uint32_t kSstMagic = 0x4C535354u;   // 'LSST' (footer)
inline constexpr std::uint32_t kManMagic = 0x4C4D414Eu;   // 'LMAN' (manifest INSTALL rec)
// Compaction (C3.4) extends the append-only manifest with two MORE record kinds,
// each a distinct magic so a decoder dispatches on the leading word (the INSTALL
// 48-byte layout is byte-UNCHANGED — backward compatible with step-3 manifests):
//   * OBSOLETE — a previously-installed SSTable is now superseded by a compaction
//     output and must NOT be loaded as live on recovery (its disk is reclaimable).
//   * WAL-TRUNCATE — the committed history up to `seq` now lives durably in the
//     SSTable set, so WAL records with seq <= this watermark need not be replayed
//     (the WAL prefix below it is reclaimable). A monotonic high-water mark.
inline constexpr std::uint32_t kManObsoleteMagic = 0x4D42534Fu;  // 'OSBM' obsolete
inline constexpr std::uint32_t kManWalTruncMagic = 0x4D52544Cu;  // 'LTRM' wal-trunc
// W2: OPTIONAL one-time manifest STREAM HEADER at offset 0: [u32 kManStreamMagic]
// [u32 version]. Distinct from every record magic so recovery self-identifies it and
// skips it before the record fold (fail-closed on an unknown version); a headerless
// (kManMagic-leading) manifest reads from offset 0 (backward-compatible).
inline constexpr std::uint32_t kManStreamMagic = 0x4D4E5453u;  // 'MNTS' (manifest stream)
inline constexpr std::size_t kSstFooterBytes = 52;        // see FOOTER layout (W2: +u32 version)
inline constexpr std::size_t kBlockCrcBytes = 4;
inline constexpr std::size_t kSstTargetBlockBytes = 256;  // small ⇒ many blocks
inline constexpr std::size_t kManRecordBytes = 4 + 8 + 8 + 8 + 8 + 8 + 4;  // 48
// OBSOLETE/WAL-TRUNCATE records: magic + entry_no + u64 payload + crc = 24 bytes.
inline constexpr std::size_t kManAuxRecordBytes = 4 + 8 + 8 + 4;  // 24

// ---------------------------------------------------------------------------
// Bloom filter — hand-rolled, deterministic. Double-hashing (Kirsch-Mitzenmacher)
// over two splitmix-style 64-bit hashes of the key bytes, giving nhash bit
// positions. NEVER a false negative: every inserted key's bits are set, so
// contains() over the SAME bits can only ever return true for a present key.
// ---------------------------------------------------------------------------
class BloomFilter {
public:
    // Build sized for n_keys at ~bits_per_key (deterministic, no float ordering
    // affecting output — only the bit count). nhash fixed at a sensible 7 (we do
    // not tune empirically here; D4 tuning is the bench agent's job).
    static BloomFilter build(const std::vector<Key>& keys, std::uint32_t bits_per_key = 10) {
        BloomFilter bf;
        std::uint32_t nbits = static_cast<std::uint32_t>(keys.size()) * bits_per_key;
        if (nbits < 64u) {
            nbits = 64u;  // a floor so tiny tables still have a usable filter
        }
        bf.nbits_ = nbits;
        bf.nhash_ = 7u;
        bf.bits_.assign((nbits + 7u) / 8u, std::byte{0});
        for (const Key& k : keys) {
            bf.add(k);
        }
        return bf;
    }

    void add(const Key& key) {
        std::uint64_t h1 = 0;
        std::uint64_t h2 = 0;
        hash_pair(key, h1, h2);
        for (std::uint32_t i = 0; i < nhash_; ++i) {
            const std::uint64_t pos = (h1 + static_cast<std::uint64_t>(i) * h2) % nbits_;
            bits_[pos >> 3] |= static_cast<std::byte>(1u << (pos & 7u));
        }
    }

    // "Maybe present" — true if ALL nhash bits are set. A present key's bits were
    // all set at build, so a present key NEVER returns false (no false negative).
    [[nodiscard]] bool maybe_contains(const Key& key) const {
        if (nbits_ == 0) {
            return true;  // an empty/degenerate filter never skips (safe).
        }
        std::uint64_t h1 = 0;
        std::uint64_t h2 = 0;
        hash_pair(key, h1, h2);
        for (std::uint32_t i = 0; i < nhash_; ++i) {
            const std::uint64_t pos = (h1 + static_cast<std::uint64_t>(i) * h2) % nbits_;
            if ((bits_[pos >> 3] & static_cast<std::byte>(1u << (pos & 7u))) == std::byte{0}) {
                return false;  // a cleared bit ⇒ definitely absent.
            }
        }
        return true;
    }

    // Serialise the payload: u32 nbits | u32 nhash | bits[].
    [[nodiscard]] std::vector<std::byte> encode() const {
        std::vector<std::byte> out;
        put_u32(out, nbits_);
        put_u32(out, nhash_);
        out.insert(out.end(), bits_.begin(), bits_.end());
        return out;
    }

    // Decode from a payload span; returns false on a malformed/short payload.
    [[nodiscard]] static bool decode(std::span<const std::byte> p, BloomFilter& out) {
        if (p.size() < 8) {
            return false;
        }
        out.nbits_ = get_u32(p.data());
        out.nhash_ = get_u32(p.data() + 4);
        const std::size_t want = (static_cast<std::size_t>(out.nbits_) + 7u) / 8u;
        if (p.size() - 8 < want || out.nbits_ == 0 || out.nhash_ == 0) {
            return false;
        }
        out.bits_.assign(p.begin() + 8, p.begin() + 8 + static_cast<std::ptrdiff_t>(want));
        return true;
    }

private:
    // Two independent 64-bit hashes of the key bytes (FNV-1a seeded two ways,
    // avalanched splitmix-style). Deterministic + portable (no std::hash, which
    // is implementation-defined and would break byte-identical replay).
    static void hash_pair(const Key& key, std::uint64_t& h1, std::uint64_t& h2) {
        std::uint64_t a = 0xCBF29CE484222325ULL;
        std::uint64_t b = 0x100000001B3ULL;
        for (char c : key) {
            const std::uint64_t byte = static_cast<std::uint8_t>(c);
            a = (a ^ byte) * 0x100000001B3ULL;
            b = (b ^ byte) * 0xCBF29CE484222325ULL;
        }
        // Avalanche each (splitmix64 finalizer) so small key diffs spread.
        a ^= a >> 33;
        a *= 0xFF51AFD7ED558CCDULL;
        a ^= a >> 33;
        b ^= b >> 29;
        b *= 0xC2B2AE3D27D4EB4FULL;
        b ^= b >> 32;
        h1 = a;
        h2 = (b | 1ULL);  // keep h2 odd so the step never collapses to 0
    }

    std::uint32_t nbits_ = 0;
    std::uint32_t nhash_ = 0;
    std::vector<std::byte> bits_;
};

// ---------------------------------------------------------------------------
// SSTableBuilder — serialise a sorted run of SstEntry into the on-disk byte
// image (data blocks + index + bloom + footer), pure + synchronous. The caller
// writes the returned bytes to an IDisk then syncs (durability is the caller's).
// ---------------------------------------------------------------------------
struct SstBuildResult {
    std::vector<std::byte> bytes;  // the full SSTable image
    Seq min_seq = kNoSeq;
    Seq max_seq = kNoSeq;
    // PERF (K4.9): the decoded reader state, captured AS the image is encoded, so the
    // engine can install a reader for its OWN freshly-built table without re-parsing
    // the bytes it just produced (profiles showed that decode+CRC of our own image as
    // a top ingest cost). Recovery still parses from disk — this is only for adopt().
    std::vector<std::vector<SstEntry>> blocks;   // entries per data block
    std::vector<std::uint64_t> block_offs;       // framed block offsets (parallel)
    std::vector<std::uint32_t> block_lens;       // framed block lengths (parallel)
    BloomFilter bloom;
};

class SSTableBuilder {
public:
    // entries MUST be key-ascending then seq-ascending (the memtable's order).
    [[nodiscard]] static SstBuildResult build(const std::vector<SstEntry>& entries) {
        SstBuildResult res;
        std::vector<std::byte>& out = res.bytes;

        // Collect distinct keys for the bloom (and compute min/max seq).
        std::vector<Key> keys;
        for (const SstEntry& e : entries) {
            if (keys.empty() || keys.back() != e.key) {
                keys.push_back(e.key);
            }
            if (res.min_seq == kNoSeq || e.seq < res.min_seq) {
                res.min_seq = e.seq;
            }
            if (e.seq > res.max_seq) {
                res.max_seq = e.seq;
            }
        }

        // --- DATA BLOCKS + sparse index records --------------------------------
        struct IndexRec {
            std::uint64_t off;
            std::uint32_t len;  // framed length (payload + crc)
            Key first_key;
        };
        std::vector<IndexRec> index;

        std::size_t i = 0;
        while (i < entries.size()) {
            std::vector<std::byte> payload;
            const Key first_key = entries[i].key;
            std::vector<SstEntry> block_entries;
            // Pack entries until the block would exceed the target (≥1 per block).
            while (i < entries.size()) {
                const SstEntry& e = entries[i];
                const std::size_t entry_size =
                    4 + 4 + 8 + 1 + e.key.size() + e.value.size();
                if (!payload.empty() && payload.size() + entry_size > kSstTargetBlockBytes) {
                    break;
                }
                encode_entry(payload, e);
                block_entries.push_back(e);
                ++i;
            }
            const std::uint64_t block_off = out.size();
            const std::uint32_t framed_len = write_block(out, payload);
            index.push_back(IndexRec{block_off, framed_len, first_key});
            res.blocks.push_back(std::move(block_entries));
            res.block_offs.push_back(block_off);
            res.block_lens.push_back(framed_len);
        }

        // --- INDEX BLOCK -------------------------------------------------------
        std::vector<std::byte> index_payload;
        for (const IndexRec& r : index) {
            put_u64(index_payload, r.off);
            put_u32(index_payload, r.len);
            put_u32(index_payload, static_cast<std::uint32_t>(r.first_key.size()));
            append_bytes(index_payload, r.first_key);
        }
        const std::uint64_t index_off = out.size();
        const std::uint32_t index_len = write_block(out, index_payload);

        // --- BLOOM BLOCK -------------------------------------------------------
        BloomFilter bloom = BloomFilter::build(keys);
        const std::vector<std::byte> bloom_payload = bloom.encode();
        res.bloom = bloom;
        const std::uint64_t bloom_off = out.size();
        const std::uint32_t bloom_len = write_block(out, bloom_payload);

        // --- FOOTER ------------------------------------------------------------
        std::vector<std::byte> footer;
        put_u64(footer, index_off);
        put_u32(footer, index_len);
        put_u64(footer, bloom_off);
        put_u32(footer, bloom_len);
        put_u64(footer, res.min_seq);
        put_u64(footer, res.max_seq);
        put_u32(footer, format::kSstableVersion);  // W2: format version (before magic)
        put_u32(footer, kSstMagic);
        const std::uint32_t fcrc =
            Crc32::compute(std::span<const std::byte>(footer.data(), footer.size()));
        put_u32(footer, fcrc);
        out.insert(out.end(), footer.begin(), footer.end());

        return res;
    }

private:
    static void append_bytes(std::vector<std::byte>& out, const std::string& s) {
        for (char c : s) {
            out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        }
    }

    static void encode_entry(std::vector<std::byte>& out, const SstEntry& e) {
        put_u32(out, static_cast<std::uint32_t>(e.key.size()));
        put_u32(out, static_cast<std::uint32_t>(e.value.size()));
        put_u64(out, e.seq);
        // flags byte: bit0 = tombstone, bit1 = vlog-pointer (WiscKey). bit0 alone
        // is the legacy 0/1 tomb encoding, so this stays backward-compatible.
        const std::uint8_t flags =
            static_cast<std::uint8_t>((e.tombstone ? 1u : 0u) | (e.vlog ? 2u : 0u));
        out.push_back(static_cast<std::byte>(flags));
        append_bytes(out, e.key);
        append_bytes(out, e.value);
    }

    // Frame a block: payload + trailing CRC over the payload. Returns framed len.
    static std::uint32_t write_block(std::vector<std::byte>& out,
                                     const std::vector<std::byte>& payload) {
        out.insert(out.end(), payload.begin(), payload.end());
        const std::uint32_t crc =
            Crc32::compute(std::span<const std::byte>(payload.data(), payload.size()));
        put_u32(out, crc);
        return static_cast<std::uint32_t>(payload.size() + kBlockCrcBytes);
    }
};

// ---------------------------------------------------------------------------
// SSTableReader — an in-memory view over a durable SSTable, loaded by reading
// the footer (last kSstFooterBytes), then the index + bloom blocks, each CRC-checked.
// load() is async (reads via IDisk). After load, lookup()/scan() are pure +
// synchronous over the cached index/bloom but read DATA blocks lazily via the
// disk — which is async — so they are coroutines too. To keep the read path
// simple + await-safe, load() eagerly reads ALL data blocks into memory after
// validating the footer/index (small tables in the sim; correctness over IO
// economy here — block-laziness is a later optimisation). Every block's CRC is
// verified on load; ANY failure rejects the whole SSTable (returns !ok), so a
// torn/partial SSTable never serves a value (V-NOTORN).
// ---------------------------------------------------------------------------
class SSTableReader {
public:
    Seq min_seq = kNoSeq;
    Seq max_seq = kNoSeq;
    std::uint64_t sstable_id = 0;

    // Newest version of `key` with seq <= at within THIS table. Returns whether a
    // version was found (covered=true) and, if so, its value/tombstone — so the
    // caller can distinguish "this table has the newest version, it's a tombstone"
    // (stop, ∅) from "this table has nothing for the key" (keep going older).
    struct Hit {
        bool covered = false;
        bool tombstone = false;
        Value value;       // inline value, OR (vlog==true) the encoded VlogPtr
        Seq seq = kNoSeq;
        bool vlog = false;  // WiscKey: `value` is a VlogPtr — the caller derefs
    };

    [[nodiscard]] Hit lookup(const Key& key, Seq at) const {
        Hit hit;
        if (!bloom_.maybe_contains(key)) {
            return hit;  // bloom says definitely absent — skip (no false negative).
        }
        // Sparse-index seek: the candidate block is the LAST block whose first_key
        // <= key (binary search over key-ascending first_keys).
        const int blk = seek_block(key);
        if (blk < 0) {
            return hit;
        }
        // A key's versions are contiguous + ascending, but with MANY versions a key
        // SPANS several blocks AND may START mid-block (its first version packed
        // after a smaller key). seek_block lands on the LAST block whose first_key
        // <= key — for a multi-block key that is the key's LAST block. We must scan
        // EVERY block of the key's run, so BACK UP to the run's first block:
        //   * over preceding blocks whose first_key == key (pure spill blocks), then
        //   * ONE more predecessor (first_key < key): the key may START in it (its
        //     first versions packed after a smaller key, spilling into block `lo`).
        // Scanning a block that does not actually hold the key is harmless (the
        // per-entry filter only matches the exact key). Without this a key whose
        // OLDEST visible version (<= at) lives in an earlier block is read as absent
        // — surfacing ∅ where a value is live (the compaction-GC multi-version case).
        std::size_t lo = static_cast<std::size_t>(blk);
        while (lo > 0 && index_[lo - 1].first_key == key) {
            --lo;
        }
        if (lo > 0 && index_[lo - 1].first_key < key) {
            --lo;  // the key may begin mid-block in this predecessor.
        }
        // Scan forward from `lo` while the block could still hold the key (its
        // first_key <= key); the first block whose first_key > key cannot (ascending).
        for (std::size_t b = lo; b < blocks_.size(); ++b) {
            if (index_[b].first_key > key) {
                break;
            }
            scan_block_for_key(b, key, at, hit);
        }
        return hit;
    }

    // Append every visible (key,value) in [lo,hi) at snapshot `at` from THIS
    // table into `acc` as (key -> (seq, value, tombstone)) candidates. The caller
    // merges across tables + the memtable picking the newest seq per key.
    struct ScanCand {
        Seq seq = kNoSeq;
        Value value;       // inline value, OR (vlog==true) the encoded VlogPtr
        bool tombstone = false;
        bool vlog = false;  // WiscKey: `value` is a VlogPtr — the caller derefs
    };
    void scan_into(const Key& lo, const Key& hi, Seq at, bool hi_unbounded,
                   std::vector<std::pair<Key, ScanCand>>& acc) const {
        // K1 perf: SEEK to the range via the sparse index instead of walking EVERY block
        // (this loop used to be O(table) per scan — a probed-list scan over a flushed
        // 100k-row table filtered ~13x more entries than it accepted). Blocks and the
        // entries within each block are key-ascending, so:
        //   * start at the LAST block whose first_key <= lo, backing up over pure-spill
        //     predecessors exactly like lookup() (a key's version run can begin
        //     mid-block after a smaller key);
        //   * stop at the first block whose first_key is past hi, and break out of a
        //     block once its entries pass hi.
        // The accepted (entry, order) sequence is IDENTICAL to the full walk's.
        std::size_t b0 = 0;
        if (!lo.empty()) {
            const int blk = seek_block(lo);
            if (blk >= 0) {
                b0 = static_cast<std::size_t>(blk);
                while (b0 > 0 && index_[b0 - 1].first_key == lo) {
                    --b0;
                }
                if (b0 > 0 && index_[b0 - 1].first_key < lo) {
                    --b0;  // lo's first versions may start mid-block in this predecessor
                }
            }
        }
        for (std::size_t b = b0; b < blocks_.size(); ++b) {
            if (!hi_unbounded && !(index_[b].first_key < hi)) {
                break;  // this block (and every later one) starts at or past hi
            }
            for (const SstEntry& e : blocks_[b]) {
                if (e.seq > at) {
                    continue;
                }
                if (e.key < lo) {
                    continue;
                }
                if (!hi_unbounded && !(e.key < hi)) {
                    break;  // entries are key-ascending — the rest of the block is past hi
                }
                acc.emplace_back(e.key, ScanCand{e.seq, e.value, e.tombstone, e.vlog});
            }
        }
    }

    // K1 perf: zero-copy cursor surface over the DECODED blocks (key-asc across blocks,
    // key-asc/seq-asc within one) — the streaming scan merge iterates entries by
    // REFERENCE instead of materialising (key, value) copies per accepted entry.
    [[nodiscard]] std::size_t block_count() const noexcept { return blocks_.size(); }
    [[nodiscard]] const std::vector<SstEntry>& block(std::size_t b) const { return blocks_[b]; }
    // The block where a scan for keys >= lo starts — the same lookup()-style seek +
    // spill back-up scan_into uses (an empty lo starts at block 0).
    [[nodiscard]] std::size_t scan_start_block(const Key& lo) const {
        if (lo.empty() || blocks_.empty()) return 0;
        const int blk = seek_block(lo);
        if (blk < 0) return 0;
        std::size_t b0 = static_cast<std::size_t>(blk);
        while (b0 > 0 && index_[b0 - 1].first_key == lo) --b0;
        if (b0 > 0 && index_[b0 - 1].first_key < lo) --b0;
        return b0;
    }

    // Append EVERY decoded entry of this table (key-asc/seq-asc) into `out` — the
    // compaction merge input (C3.4). Pure, synchronous over the cached blocks.
    void collect_entries(std::vector<SstEntry>& out) const {
        for (const std::vector<SstEntry>& blk : blocks_) {
            for (const SstEntry& e : blk) {
                out.push_back(e);
            }
        }
    }

    // Total decoded version count across all blocks (the GC-space metric).
    [[nodiscard]] std::size_t entry_count() const noexcept {
        std::size_t n = 0;
        for (const std::vector<SstEntry>& blk : blocks_) {
            n += blk.size();
        }
        return n;
    }

private:
    friend class SSTableLoader;

    struct IndexRec {
        std::uint64_t off = 0;
        std::uint32_t len = 0;
        Key first_key;
    };

    // The LAST block index whose first_key <= key, or -1 if key precedes block 0.
    [[nodiscard]] int seek_block(const Key& key) const {
        int lo = 0;
        int hi = static_cast<int>(index_.size()) - 1;
        int ans = -1;
        while (lo <= hi) {
            const int mid = lo + (hi - lo) / 2;
            if (index_[static_cast<std::size_t>(mid)].first_key <= key) {
                ans = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return ans;
    }

    void scan_block_for_key(std::size_t b, const Key& key, Seq at, Hit& hit) const {
        for (const SstEntry& e : blocks_[b]) {
            if (e.key != key) {
                continue;
            }
            if (e.seq <= at && e.seq >= hit.seq) {
                hit.covered = true;
                hit.seq = e.seq;
                hit.tombstone = e.tombstone;
                hit.value = e.value;
                hit.vlog = e.vlog;
            }
        }
    }

    std::vector<IndexRec> index_;
    std::vector<std::vector<SstEntry>> blocks_;  // decoded data blocks (parallel)
    BloomFilter bloom_;

public:
    // PERF (K4.9): install this reader from the builder's own output — the state the
    // loader would decode from `r.bytes`, captured at encode time instead. MUST be
    // byte-equivalent to SSTableLoader::parse(r.bytes, id, *this); the differential
    // gate in storage_sstable_test compares the two on every surface. Recovery paths
    // never use this (they parse real disk bytes, CRC and all).
    // Whole-table key bounds (nullptr when empty) — used by compaction to detect
    // pure topic-namespace segments that never need merging.
    [[nodiscard]] const Key* first_key() const noexcept {
        for (const auto& b : blocks_)
            if (!b.empty()) return &b.front().key;
        return nullptr;
    }
    [[nodiscard]] const Key* last_key() const noexcept {
        for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it)
            if (!it->empty()) return &it->back().key;
        return nullptr;
    }

    void adopt_built(const SstBuildResult& r, std::uint64_t id) {
        sstable_id = id;
        min_seq = r.min_seq;
        max_seq = r.max_seq;
        bloom_ = r.bloom;
        blocks_ = r.blocks;
        index_.clear();
        for (std::size_t b = 0; b < r.blocks.size(); ++b) {
            index_.push_back(IndexRec{r.block_offs[b], r.block_lens[b],
                                      r.blocks[b].empty() ? Key{} : r.blocks[b].front().key});
        }
    }
};

// ---------------------------------------------------------------------------
// COMPACTION MERGE + VERSION GC (C3.4 / V-GC). Pure, synchronous: given the
// decoded entry runs of several SSTables (oldest→newest install order) and the
// GC watermark (the oldest live snapshot Seq), produce ONE key-asc/seq-asc entry
// run with dead versions dropped — ready to feed SSTableBuilder::build.
//
// K-WAY MERGE: every input run is key-asc/seq-asc; we concatenate all entries and
// sort by (key asc, seq asc). A (key,seq) pair is GLOBALLY UNIQUE (Seq is the
// monotonic commit id; the same version may appear in >1 SSTable after a flush+
// replay, so we DEDUP identical (key,seq) keeping one copy).
//
// VERSION GC RULE (V-GC — the binding contract):
//   watermark = the oldest live snapshot Seq (single node). For a key k with
//   versions v1<v2<…<vn (by seq), a version vi is DROPPABLE iff there exists a
//   newer version vj (j>i) with vj.seq <= watermark. Reason: every live snapshot
//   reads as-of some seq >= watermark, so it sees vj (or newer) — never vi. The
//   newest version with seq <= watermark is the OLDEST one any live snapshot can
//   still observe; everything strictly older than it is dead and dropped. We keep
//   that survivor AND every version with seq > watermark (a future/younger
//   snapshot or the live tip may need them). NEVER drop a version still visible:
//   if no version has seq <= watermark we keep them ALL (the watermark predates
//   the key entirely).
//   TOMBSTONE GC: a tombstone that becomes the OLDEST retained version of its key
//   AND is at/below the watermark covers "absent for every live snapshot" — it
//   can be dropped entirely (no older value to resurrect, nothing newer to need
//   it as a delimiter). We only drop it when it is the survivor (the newest
//   version <= watermark) AND it is a tombstone AND there is no retained version
//   above it that is a value needing the delete boundary — i.e. it is the single
//   surviving floor; then the key vanishes for snapshots < its next version.
//   Conservative + correct: we KEEP a tombstone if any version with seq>watermark
//   exists for the key (a mid-history snapshot between them must still read ∅).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<SstEntry> compact_merge(
    const std::vector<std::vector<SstEntry>>& runs, Seq watermark) {
    // (1) gather every entry.
    std::vector<SstEntry> all;
    for (const std::vector<SstEntry>& run : runs) {
        for (const SstEntry& e : run) {
            all.push_back(e);
        }
    }
    // (2) sort by (key asc, seq asc). Deterministic comparator — no hashing.
    std::sort(all.begin(), all.end(), [](const SstEntry& a, const SstEntry& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        return a.seq < b.seq;
    });
    // (3) dedup identical (key,seq) — a version flushed then WAL-replayed can
    // appear twice; they are byte-identical, keep one.
    std::vector<SstEntry> uniq;
    for (SstEntry& e : all) {
        if (!uniq.empty() && uniq.back().key == e.key && uniq.back().seq == e.seq) {
            continue;
        }
        uniq.push_back(std::move(e));
    }
    // (4) per-key GC under the watermark. Walk each key's ascending versions.
    std::vector<SstEntry> out;
    std::size_t i = 0;
    while (i < uniq.size()) {
        std::size_t j = i;
        while (j < uniq.size() && uniq[j].key == uniq[i].key) {
            ++j;
        }
        // versions [i,j) for one key, seq-ascending. Find the survivor floor: the
        // index of the NEWEST version with seq <= watermark (or -1 if none).
        std::ptrdiff_t floor = -1;
        for (std::size_t k = i; k < j; ++k) {
            if (uniq[k].seq <= watermark) {
                floor = static_cast<std::ptrdiff_t>(k);
            } else {
                break;  // ascending ⇒ the rest are > watermark
            }
        }
        // Keep: the floor survivor (if any) + everything above the watermark.
        // Everything strictly below the floor is dead (no live snapshot sees it).
        const std::size_t keep_from =
            (floor < 0) ? i : static_cast<std::size_t>(floor);
        // TOMBSTONE GC: if the floor survivor is a tombstone AND there is NO
        // retained version above it (the whole key is just this delete, fully
        // below the watermark), the key is "absent for every live snapshot" — drop
        // the tombstone entirely (it reclaims the key). Only safe when the floor
        // is the LAST version (nothing newer that a snapshot between could read).
        bool drop_lone_tombstone = false;
        if (floor >= 0) {
            const std::size_t fl = static_cast<std::size_t>(floor);
            if (uniq[fl].tombstone && fl + 1 == j) {
                drop_lone_tombstone = true;
            }
        }
        if (!drop_lone_tombstone) {
            for (std::size_t k = keep_from; k < j; ++k) {
                out.push_back(uniq[k]);
            }
        } else {
            // Drop the lone surviving tombstone (and the dead older versions
            // already excluded). Nothing emitted for this key.
        }
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// SSTableLoader — the async loader. Reads + validates an SSTable image from an
// IDisk into an SSTableReader. Trusts only the durable length `sst_len` recorded
// in the manifest (so a torn tail past it is never read). Returns !ok on ANY
// integrity failure → the SSTable is rejected (V-NOTORN); recovery treats a
// rejected SSTable the same as one that was never installed.
// ---------------------------------------------------------------------------
class SSTableLoader {
public:
    // Read the whole [0, sst_len) image, parse footer/index/bloom/data, verifying
    // every block CRC. On success fills `out` and returns ok.
    [[nodiscard]] static core::Task load(IDisk& disk, std::uint64_t sst_len,
                                         std::uint64_t sstable_id, SSTableReader& out,
                                         Error& result_out) {
        result_out = Error{ErrorCode::Corruption, "sstable: empty/short image"};
        if (sst_len < kSstFooterBytes) {
            co_return;
        }
        std::vector<std::byte> image(sst_len);
        const Error re =
            co_await disk.read(0, std::span<std::byte>(image.data(), image.size()));
        if (!re.ok()) {
            result_out = re;  // a covered bit-rot / io-fault → reject the table.
            co_return;
        }
        if (parse(image, sstable_id, out)) {
            result_out = Error{};
        }
        co_return;
    }

    // Pure parse/validate of a complete image (exposed for unit tests). Returns
    // false on any framing/CRC failure.
    [[nodiscard]] static bool parse(const std::vector<std::byte>& image,
                                    std::uint64_t sstable_id, SSTableReader& out) {
        const std::size_t n = image.size();
        if (n < kSstFooterBytes) {
            return false;
        }
        // Footer layout (52 bytes): index_off[0..8) index_len[8..12)
        //   bloom_off[12..20) bloom_len[20..24) min_seq[24..32) max_seq[32..40)
        //   version[40..44) magic[44..48) crc[48..52).
        const std::byte* f = image.data() + (n - kSstFooterBytes);
        if (get_u32(f + 44) != kSstMagic) {
            return false;
        }
        const std::uint32_t want_crc = get_u32(f + 48);
        const std::uint32_t got_crc =
            Crc32::compute(std::span<const std::byte>(f, kSstFooterBytes - 4));
        if (want_crc != got_crc) {
            return false;
        }
        // W2: refuse an unknown (future) format version, fail-closed — a newer-format
        // SSTable is never mis-decoded; it is rejected exactly like a corrupt table
        // (recovery stops at it, honoring Seq-contiguity). CRC is validated first so a
        // bit-flipped version field reads as corruption, not a spurious version refuse.
        if (get_u32(f + 40) != format::kSstableVersion) {
            return false;
        }
        const std::uint64_t index_off = get_u64(f + 0);
        const std::uint32_t index_len = get_u32(f + 8);
        const std::uint64_t bloom_off = get_u64(f + 12);
        const std::uint32_t bloom_len = get_u32(f + 20);
        out.min_seq = get_u64(f + 24);
        out.max_seq = get_u64(f + 32);
        out.sstable_id = sstable_id;

        // --- read index block (validate CRC) ---
        std::vector<std::byte> ip;
        if (!read_block(image, index_off, index_len, ip)) {
            return false;
        }
        if (!parse_index(ip, out)) {
            return false;
        }

        // --- read bloom block (validate CRC) ---
        std::vector<std::byte> bp;
        if (!read_block(image, bloom_off, bloom_len, bp)) {
            return false;
        }
        if (!BloomFilter::decode(std::span<const std::byte>(bp.data(), bp.size()), out.bloom_)) {
            return false;
        }

        // --- read + decode each DATA block (validate CRC) ---
        out.blocks_.clear();
        out.blocks_.reserve(out.index_.size());
        for (const SSTableReader::IndexRec& r : out.index_) {
            std::vector<std::byte> dp;
            if (!read_block(image, r.off, r.len, dp)) {
                return false;
            }
            std::vector<SstEntry> entries;
            if (!decode_data_block(dp, entries)) {
                return false;
            }
            out.blocks_.push_back(std::move(entries));
        }
        return true;
    }

private:
    // Read a framed block [off, off+len) and verify its trailing CRC; copy the
    // payload (len - 4 bytes) into `payload_out`. Returns false on bounds/CRC.
    static bool read_block(const std::vector<std::byte>& image, std::uint64_t off,
                           std::uint32_t len, std::vector<std::byte>& payload_out) {
        if (len < kBlockCrcBytes) {
            return false;
        }
        const std::uint64_t end = off + len;
        if (end > image.size()) {
            return false;
        }
        const std::byte* p = image.data() + off;
        const std::size_t plen = len - kBlockCrcBytes;
        const std::uint32_t want = get_u32(p + plen);
        const std::uint32_t got = Crc32::compute(std::span<const std::byte>(p, plen));
        if (want != got) {
            return false;
        }
        payload_out.assign(p, p + plen);
        return true;
    }

    static bool parse_index(const std::vector<std::byte>& ip, SSTableReader& out) {
        out.index_.clear();
        std::size_t pos = 0;
        while (pos < ip.size()) {
            if (pos + 16 > ip.size()) {
                return false;
            }
            SSTableReader::IndexRec r;
            r.off = get_u64(ip.data() + pos);
            r.len = get_u32(ip.data() + pos + 8);
            const std::uint32_t klen = get_u32(ip.data() + pos + 12);
            pos += 16;
            if (pos + klen > ip.size()) {
                return false;
            }
            r.first_key.assign(reinterpret_cast<const char*>(ip.data() + pos), klen);
            pos += klen;
            out.index_.push_back(std::move(r));
        }
        return true;
    }

    static bool decode_data_block(const std::vector<std::byte>& dp,
                                  std::vector<SstEntry>& entries) {
        std::size_t pos = 0;
        while (pos < dp.size()) {
            if (pos + 17 > dp.size()) {
                return false;
            }
            const std::uint32_t klen = get_u32(dp.data() + pos);
            const std::uint32_t vlen = get_u32(dp.data() + pos + 4);
            const std::uint64_t seq = get_u64(dp.data() + pos + 8);
            const std::uint8_t flags = std::to_integer<std::uint8_t>(dp[pos + 16]);
            pos += 17;
            const std::uint64_t body = static_cast<std::uint64_t>(klen) + vlen;
            if (pos + body > dp.size()) {
                return false;
            }
            SstEntry e;
            e.seq = seq;
            e.tombstone = (flags & 1u) != 0u;
            e.vlog = (flags & 2u) != 0u;
            e.key.assign(reinterpret_cast<const char*>(dp.data() + pos), klen);
            e.value.assign(reinterpret_cast<const char*>(dp.data() + pos + klen), vlen);
            pos += static_cast<std::size_t>(body);
            entries.push_back(std::move(e));
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Manifest records (decoded) + (de)serialise. The append-only atomic-install +
// compaction log. THREE record kinds share one entry_no sequence (1,2,3,…):
//   * Install   — adds an SSTable to the live set (the step-3 48-byte format).
//   * Obsolete  — removes a previously-installed SSTable (compaction superseded
//     it). Its backing disk is reclaimable once this record is durable.
//   * WalTrunc  — raises the WAL-truncation watermark: WAL records with seq <=
//     `seq` are covered by the durable SSTable set and need not be replayed.
// ---------------------------------------------------------------------------
enum class ManifestKind : std::uint8_t { Install, Obsolete, WalTrunc };

struct ManifestRecord {
    ManifestKind kind = ManifestKind::Install;
    std::uint64_t entry_no = 0;   // 1,2,3,… (Seq-contiguity across ALL kinds)
    std::uint64_t sstable_id = 0; // Install/Obsolete: the SSTable backing id
    std::uint64_t sst_len = 0;    // Install: durable image length
    Seq min_seq = kNoSeq;         // Install: covered seq range
    Seq max_seq = kNoSeq;
    Seq wal_trunc_seq = kNoSeq;   // WalTrunc: the truncation watermark
};

// Encode an INSTALL record (the step-3 byte-exact 48-byte layout; UNCHANGED).
[[nodiscard]] inline std::vector<std::byte> encode_manifest(const ManifestRecord& r) {
    std::vector<std::byte> buf;
    put_u32(buf, kManMagic);
    put_u64(buf, r.entry_no);
    put_u64(buf, r.sstable_id);
    put_u64(buf, r.sst_len);
    put_u64(buf, r.min_seq);
    put_u64(buf, r.max_seq);
    const std::uint32_t crc = Crc32::compute(std::span<const std::byte>(buf.data(), buf.size()));
    put_u32(buf, crc);
    return buf;
}

// Encode an OBSOLETE record: magic | entry_no | sstable_id | crc (24 bytes).
[[nodiscard]] inline std::vector<std::byte> encode_manifest_obsolete(std::uint64_t entry_no,
                                                                     std::uint64_t sstable_id) {
    std::vector<std::byte> buf;
    put_u32(buf, kManObsoleteMagic);
    put_u64(buf, entry_no);
    put_u64(buf, sstable_id);
    const std::uint32_t crc = Crc32::compute(std::span<const std::byte>(buf.data(), buf.size()));
    put_u32(buf, crc);
    return buf;
}

// Encode a WAL-TRUNCATE record: magic | entry_no | wal_trunc_seq | crc (24 bytes).
[[nodiscard]] inline std::vector<std::byte> encode_manifest_wal_trunc(std::uint64_t entry_no,
                                                                      Seq wal_trunc_seq) {
    std::vector<std::byte> buf;
    put_u32(buf, kManWalTruncMagic);
    put_u64(buf, entry_no);
    put_u64(buf, wal_trunc_seq);
    const std::uint32_t crc = Crc32::compute(std::span<const std::byte>(buf.data(), buf.size()));
    put_u32(buf, crc);
    return buf;
}

struct ManifestDecode {
    bool ok = false;
    ManifestRecord record;
    std::size_t consumed = 0;
};

// Decode ONE manifest record at image[pos..], dispatching on the leading magic.
// ok=false on short/torn/bad-magic/CRC-fail — the recover loop treats it as the
// install-prefix boundary (stop-at-first-corrupt + entry_no Seq-contiguity).
[[nodiscard]] inline ManifestDecode decode_manifest(const std::vector<std::byte>& image,
                                                    std::size_t pos) {
    ManifestDecode d;
    const std::size_t avail = image.size() - pos;
    if (avail < 4) {
        return d;
    }
    const std::byte* p = image.data() + pos;
    const std::uint32_t magic = get_u32(p);
    if (magic == kManMagic) {
        if (avail < kManRecordBytes) {
            return d;
        }
        const std::size_t body = kManRecordBytes - 4;
        const std::uint32_t want = get_u32(p + body);
        const std::uint32_t got = Crc32::compute(std::span<const std::byte>(p, body));
        if (want != got) {
            return d;
        }
        ManifestRecord r;
        r.kind = ManifestKind::Install;
        r.entry_no = get_u64(p + 4);
        r.sstable_id = get_u64(p + 12);
        r.sst_len = get_u64(p + 20);
        r.min_seq = get_u64(p + 28);
        r.max_seq = get_u64(p + 36);
        d.ok = true;
        d.record = r;
        d.consumed = kManRecordBytes;
        return d;
    }
    if (magic == kManObsoleteMagic || magic == kManWalTruncMagic) {
        if (avail < kManAuxRecordBytes) {
            return d;
        }
        const std::size_t body = kManAuxRecordBytes - 4;
        const std::uint32_t want = get_u32(p + body);
        const std::uint32_t got = Crc32::compute(std::span<const std::byte>(p, body));
        if (want != got) {
            return d;
        }
        ManifestRecord r;
        r.entry_no = get_u64(p + 4);
        const std::uint64_t payload = get_u64(p + 12);
        if (magic == kManObsoleteMagic) {
            r.kind = ManifestKind::Obsolete;
            r.sstable_id = payload;
        } else {
            r.kind = ManifestKind::WalTrunc;
            r.wal_trunc_seq = payload;
        }
        d.ok = true;
        d.record = r;
        d.consumed = kManAuxRecordBytes;
        return d;
    }
    return d;  // unknown magic — corrupt/garbage, stop.
}

}  // namespace lockstep::storage
