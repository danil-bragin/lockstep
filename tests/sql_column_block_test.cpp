// sql_column_block_test.cpp — DURABLE columnar block (SoA chunk) codec gate. The
// columnar storage engine (option A) stores a column as SoA blocks (ColumnBlock.hpp).
// This proves the byte-exact serialisation is correct + deterministic IN ISOLATION
// (no engine/storage) BEFORE the write/read/flush path is wired, so a format bug can
// never reach the durable core:
//   (A) ROUND-TRIP: encode_column_block -> decode_column_block -> at(r) reproduces every
//       cell for INT (incl. negative / INT64 extremes), TEXT (incl. empty / embedded
//       0x00 / varied length), and typed NULL, in mixed columns.
//   (B) DETERMINISM: encoding the same column twice yields byte-identical bytes (the
//       durable-format determinism the gate + recovery rely on).
//   (C) EMPTY + ALL-NULL chunks round-trip.
//
// Determinism: pure functions; only entropy is an inlined SplitMix seed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/query/sql/ColumnBlock.hpp>

using namespace lockstep::query::sql;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

class SplitMix {
public:
    explicit SplitMix(std::uint64_t s) : s_(s) {}
    std::uint64_t next() {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
private:
    std::uint64_t s_;
};

bool datum_eq(const Datum& a, const Datum& b) {
    if (a.is_null || b.is_null) {
        return a.is_null && b.is_null && a.type == b.type;
    }
    if (a.type != b.type) {
        return false;
    }
    return a.type == Type::Int ? a.i == b.i : a.s == b.s;
}

void roundtrip(Type type, const std::vector<Datum>& cells, const std::string& tag) {
    const std::string enc = encode_column_block(type, cells);
    const ColumnChunk c = decode_column_block(enc);
    check(c.count == cells.size(), tag + ": count");
    bool ok = c.count == cells.size();
    for (std::uint32_t r = 0; ok && r < c.count; ++r) {
        ok = datum_eq(c.at(r), cells[r]);
    }
    check(ok, tag + ": cell round-trip");
    // determinism: re-encode identical bytes.
    check(encode_column_block(type, cells) == enc, tag + ": deterministic bytes");
}

void test_int() {
    SplitMix rng(0xC0FFEE);
    std::vector<Datum> cells;
    const std::int64_t edge[] = {0, 1, -1, INT64_MAX, INT64_MIN, 1234567, -7654321};
    for (std::int64_t e : edge) {
        cells.push_back(Datum::make_int(e));
    }
    for (int k = 0; k < 500; ++k) {
        if (rng.below(5) == 0) {
            cells.push_back(Datum::make_null(Type::Int));
        } else {
            cells.push_back(Datum::make_int(static_cast<std::int64_t>(rng.next())));
        }
    }
    roundtrip(Type::Int, cells, "int-mixed");
}

void test_text() {
    SplitMix rng(0xBEEF);
    std::vector<Datum> cells;
    cells.push_back(Datum::make_text(""));
    cells.push_back(Datum::make_text(std::string("a\0b\0c", 5)));  // embedded NULs
    cells.push_back(Datum::make_text(std::string(300, 'z')));
    cells.push_back(Datum::make_null(Type::Text));
    for (int k = 0; k < 500; ++k) {
        if (rng.below(6) == 0) {
            cells.push_back(Datum::make_null(Type::Text));
        } else {
            cells.push_back(Datum::make_text("s-" + std::to_string(rng.below(100000))));
        }
    }
    roundtrip(Type::Text, cells, "text-mixed");
}

void test_edge() {
    roundtrip(Type::Int, {}, "int-empty");
    roundtrip(Type::Text, {}, "text-empty");
    roundtrip(Type::Int, {Datum::make_null(Type::Int), Datum::make_null(Type::Int)},
              "int-all-null");
    roundtrip(Type::Text, {Datum::make_null(Type::Text)}, "text-all-null");
}

}  // namespace

int main() {
    test_int();
    test_text();
    test_edge();
    if (g_fail == 0) {
        std::printf("sql_column_block_test: ALL PASS\n");
    }
    return g_fail;
}
