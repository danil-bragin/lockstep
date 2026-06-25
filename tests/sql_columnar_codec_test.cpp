// sql_columnar_codec_test.cpp — COLUMNAR KEY-SCHEMA codec gate (PERF_PLAN Phase 6,
// columnar layout, step 1 of the durability-gated rollout).
//
// The columnar layout stores each (row, column) as its own KV under
//   col_key = "c" ++ be32(table_id) ++ ":" ++ be32(col_id) ++ ":" ++ encode_pk(pk)
// with a per-column value (kNullTag | 0x00 present + payload). This gate proves the
// pure codec is correct IN ISOLATION (no engine / no storage) BEFORE any write/read
// path is wired, so a layout bug can never reach the durable core:
//   (A) VALUE ROUND-TRIP: encode_col_value -> decode_col_value is identity for INT
//       (incl. negative / zero / INT64 extremes), TEXT (incl. empty / embedded 0x00),
//       and typed NULL (INT + TEXT).
//   (B) KEY GROUPING: every value of ONE column is a CONTIGUOUS byte range — col i's
//       keys all sort before col (i+1)'s, and a column-family range [lo,hi) contains
//       exactly that column's keys (the projection-pushdown invariant).
//   (C) KEY PK-ORDER: within one column family, keys sort in PK order (INT order-
//       preserving incl. negatives-before-positives; TEXT lexicographic).
//   (D) PK ROUND-TRIP: decode_pk_from_col_key recovers the PK datum from a col key.
//   (E) NAMESPACE DISJOINT: the columnar namespace ('c') never overlaps the row ('t')
//       or index ('i') namespaces — a columnar scan can't see row/index keys.
//
// Determinism: pure functions; no clock/rng/threads. Exit non-zero on first failure.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/query/sql/Catalog.hpp>

using namespace lockstep::query::sql;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_fail = 1;
    }
}

// A minimal Table with a single-column PK of the given type (id is the only thing the
// codec reads via t.pk()). col ids are schema indices; the codec is schema-agnostic
// beyond the PK type, so this suffices for the pure-codec gate.
Table make_table(std::uint32_t id, Type pk_type) {
    Table t;
    t.name = "tbl";
    t.id = id;
    t.columns.push_back(Column{"id", pk_type, false});
    t.columns.push_back(Column{"a", Type::Int, true});
    t.columns.push_back(Column{"b", Type::Text, true});
    t.pk_index = 0;
    return t;
}

void test_value_roundtrip() {
    const std::int64_t ints[] = {0,        1,         -1,       42,
                                 -42,      1000000,   -1000000, INT64_MAX,
                                 INT64_MIN};
    for (std::int64_t v : ints) {
        const Datum d = Datum::make_int(v);
        const Datum r = decode_col_value(Type::Int, encode_col_value(d));
        check(!r.is_null && r.type == Type::Int && r.i == v, "INT value round-trip");
    }
    const std::string texts[] = {"", "hello", std::string("a\0b", 3), "\xFE\xFF",
                                 std::string(200, 'x')};
    for (const std::string& s : texts) {
        const Datum d = Datum::make_text(s);
        const Datum r = decode_col_value(Type::Text, encode_col_value(d));
        check(!r.is_null && r.type == Type::Text && r.s == s, "TEXT value round-trip");
    }
    // typed NULL round-trips and keeps its column type.
    const Datum ni = decode_col_value(Type::Int, encode_col_value(Datum::make_null(Type::Int)));
    check(ni.is_null && ni.type == Type::Int, "INT NULL round-trip");
    const Datum nt = decode_col_value(Type::Text, encode_col_value(Datum::make_null(Type::Text)));
    check(nt.is_null && nt.type == Type::Text, "TEXT NULL round-trip");
}

void test_key_grouping_and_order() {
    const Table t = make_table(7, Type::Int);
    // (B) column grouping: col 0's keys all sort before col 1's, all before col 2's,
    // and each lands inside its own [col_prefix, col_prefix_end) family range.
    for (std::uint32_t col = 0; col < 3; ++col) {
        const std::string lo = col_prefix(t.id, col);
        const std::string hi = col_prefix_end(t.id, col);
        for (std::int64_t pk : {INT64_MIN, INT64_C(-5), INT64_C(0), INT64_C(5), INT64_MAX}) {
            const std::string k = col_key(t.id, col, Datum::make_int(pk));
            check(k >= lo && k < hi, "col key inside its family range");
            if (col + 1 < 3) {
                const std::string nxt = col_key(t.id, col + 1, Datum::make_int(pk));
                check(k < nxt, "col i keys sort before col i+1 keys");
            }
        }
    }
    // (C) within one family, keys sort in PK order (negatives before positives).
    const std::int64_t order[] = {INT64_MIN, -1000, -1, 0, 1, 1000, INT64_MAX};
    for (std::size_t i = 1; i < sizeof(order) / sizeof(order[0]); ++i) {
        const std::string prev = col_key(t.id, 1, Datum::make_int(order[i - 1]));
        const std::string cur = col_key(t.id, 1, Datum::make_int(order[i]));
        check(prev < cur, "within-family PK order (INT)");
    }
    // TEXT PK family: lexicographic order.
    const Table tt = make_table(7, Type::Text);
    const char* ts[] = {"", "a", "aa", "ab", "b", "z"};
    for (std::size_t i = 1; i < sizeof(ts) / sizeof(ts[0]); ++i) {
        const std::string prev = col_key(tt.id, 1, Datum::make_text(ts[i - 1]));
        const std::string cur = col_key(tt.id, 1, Datum::make_text(ts[i]));
        check(prev < cur, "within-family PK order (TEXT)");
    }
}

void test_pk_roundtrip() {
    const Table ti = make_table(3, Type::Int);
    for (std::int64_t pk : {INT64_MIN, INT64_C(-7), INT64_C(0), INT64_C(7), INT64_MAX}) {
        for (std::uint32_t col = 0; col < 3; ++col) {
            const Datum r = decode_pk_from_col_key(ti, col_key(ti.id, col, Datum::make_int(pk)));
            check(!r.is_null && r.type == Type::Int && r.i == pk, "INT PK from col key");
        }
    }
    const Table tt = make_table(3, Type::Text);
    for (const std::string& pk : {std::string(""), std::string("alice"), std::string("z")}) {
        const Datum r = decode_pk_from_col_key(tt, col_key(tt.id, 2, Datum::make_text(pk)));
        check(!r.is_null && r.type == Type::Text && r.s == pk, "TEXT PK from col key");
    }
}

void test_namespace_disjoint() {
    // 'c' (columnar) must be disjoint from 't' (rows) and 'i' (indexes) for the same
    // table id, so a columnar table-scan never sees row or index keys and vice versa.
    const std::uint32_t id = 9;
    const Table t = make_table(id, Type::Int);
    const std::string ck = col_key(id, 1, Datum::make_int(123));
    const std::string rk = encode_key(t, Datum::make_int(123));   // row-mode 't' key
    const std::string ik = index_prefix(id, 1);                   // index 'i' key prefix
    check(ck[0] == 'c' && rk[0] == 't' && ik[0] == 'i', "namespace tag bytes");
    const std::string clo = col_table_prefix(id), chi = col_table_prefix_end(id);
    check(ck >= clo && ck < chi, "col key inside table namespace");
    check(!(rk >= clo && rk < chi), "row key outside columnar namespace");
    check(!(ik >= clo && ik < chi), "index key outside columnar namespace");
}

}  // namespace

int main() {
    test_value_roundtrip();
    test_key_grouping_and_order();
    test_pk_roundtrip();
    test_namespace_disjoint();
    if (g_fail == 0) {
        std::printf("sql_columnar_codec_test: ALL PASS\n");
    }
    return g_fail;
}
