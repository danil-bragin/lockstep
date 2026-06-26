// sql_json_test.cpp — F13 JSON type (logical 11 over physical TEXT). The CANONICAL form is stored
// (sorted object keys, compact, normalized numbers) so equal documents are byte-identical across
// nodes (the byte-determinism contract). No float is parsed (numbers canonicalized as strings).
// Access via `->` (JSON) / `->>` (text); json_array_length.
#include <cstdio>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::string cell0(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.render() : std::string("<none>");
}
}  // namespace

int main() {
    // canonicalization: keys sorted, whitespace stripped, numbers normalized.
    {
        SqlEngine e;
        e.exec("CREATE TABLE j (id INT, doc JSON NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO j (id, doc) VALUES (1, '{ \"b\": 1, \"a\": 2 }')").ok, "insert json");
        check(cell0(e.exec("SELECT doc FROM j WHERE id = 1")) == "{\"a\":2,\"b\":1}",
              "keys sorted + compact");
        // number normalization: 1.50 -> 1.5, 007 -> 7, -0 -> 0.
        check(e.exec("INSERT INTO j (id, doc) VALUES (2, '{\"x\": 1.50, \"y\": 007, \"z\": -0}')").ok,
              "insert numbers");
        check(cell0(e.exec("SELECT doc FROM j WHERE id = 2")) == "{\"x\":1.5,\"y\":7,\"z\":0}",
              "numbers normalized");
        // two equal docs (different spelling/order) store byte-identically.
        e.exec("INSERT INTO j (id, doc) VALUES (3, '{\"a\":2,\"b\":1}')");
        check(cell0(e.exec("SELECT doc FROM j WHERE id = 1")) ==
                  cell0(e.exec("SELECT doc FROM j WHERE id = 3")),
              "equal docs canonicalize identically");
        // teeth: invalid JSON rejected.
        check(!e.exec("INSERT INTO j (id, doc) VALUES (4, '{not json}')").ok, "invalid JSON rejected");
        check(!e.exec("INSERT INTO j (id, doc) VALUES (5, '{\"a\":1} junk')").ok, "trailing junk rejected");
    }
    // -> / ->> access (object + array).
    {
        SqlEngine e;
        e.exec("CREATE TABLE u (id INT, profile JSON NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO u (id, profile) VALUES "
               "(1, '{\"name\": \"alice\", \"age\": 30, \"tags\": [\"x\", \"y\"], \"addr\": {\"city\": \"NYC\"}}')");
        // ->> gets text (a JSON string -> its raw text).
        check(cell0(e.exec("SELECT profile ->> 'name' FROM u WHERE id = 1")) == "alice", "->> 'name'");
        check(cell0(e.exec("SELECT profile ->> 'age' FROM u WHERE id = 1")) == "30", "->> 'age' (number as text)");
        // -> gets JSON (a sub-object / array, canonical).
        check(cell0(e.exec("SELECT profile -> 'addr' FROM u WHERE id = 1")) == "{\"city\":\"NYC\"}",
              "-> 'addr' sub-object");
        // chained: -> then ->>.
        check(cell0(e.exec("SELECT profile -> 'addr' ->> 'city' FROM u WHERE id = 1")) == "NYC",
              "-> 'addr' ->> 'city'");
        // array index (0-based) + json_array_length.
        check(cell0(e.exec("SELECT profile -> 'tags' ->> 0 FROM u WHERE id = 1")) == "x", "tags[0] = x");
        check(cell0(e.exec("SELECT json_array_length(profile -> 'tags') FROM u WHERE id = 1")) == "2",
              "json_array_length(tags) = 2");
        // a missing key -> NULL.
        check(cell0(e.exec("SELECT profile ->> 'missing' FROM u WHERE id = 1")) == "NULL",
              "missing key -> NULL");
    }
    // durable: canonical JSON survives a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE j (id INT, doc JSON NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO j (id, doc) VALUES (1, '{\"z\":3,\"a\":[1,2,3]}')");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(cell0(rec.exec("SELECT doc FROM j WHERE id = 1")) == "{\"a\":[1,2,3],\"z\":3}",
              "recovered canonical JSON");
        check(cell0(rec.exec("SELECT doc -> 'a' ->> 1 FROM j WHERE id = 1")) == "2",
              "recovered JSON access");
    }
    if (g_fail) { std::printf("sql_json_test: FAILED\n"); return 1; }
    std::printf("sql_json_test: OK (JSON: canonical store, ->/->>/array access, json_array_length, "
                "durable)\n");
    return 0;
}
