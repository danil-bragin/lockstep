// sql_uuid_test.cpp — F9c UUID logical type over physical TEXT. A UUID value is a validated,
// canonicalised (lowercase, dashed) 36-char string; storage/keys/comparison stay TEXT, so the
// byte-determinism contract is untouched. Two value sources: client-supplied (validated) and a
// DETERMINISTIC DEFAULT gen_uuid() derived from a per-table counter (NOT random — random would
// diverge across the two Raft impls and break the cross-check). Teeth: malformed rejected;
// canonicalisation; generated id is a valid v4 shape; reproducible; durable.
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
bool valid_v4_shape(const std::string& u) {
    if (u.size() != 36) return false;
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (u[i] != '-') return false; continue; }
        const char c = u[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return u[14] == '4' && (u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
}
// Run the gen_uuid() script on a fresh engine and return the generated token.
std::string gen_token() {
    SqlEngine e;
    e.exec("CREATE TABLE g (gid INT, token UUID DEFAULT gen_uuid(), PRIMARY KEY (gid))");
    e.exec("INSERT INTO g (gid) VALUES (1)");
    return cell0(e.exec("SELECT token FROM g WHERE gid = 1"));
}
}  // namespace

int main() {
    // client-supplied: validate + canonicalise (lowercase), WHERE coercion canonicalises too.
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE u (id INT, uid UUID NOT NULL, PRIMARY KEY (id))").ok,
              "create UUID column");
        check(e.exec("INSERT INTO u (id, uid) VALUES "
                     "(1, '550E8400-E29B-41D4-A716-446655440000')").ok,
              "insert uppercase UUID");
        check(cell0(e.exec("SELECT uid FROM u WHERE id = 1")) ==
                  "550e8400-e29b-41d4-a716-446655440000",
              "stored lowercase-canonical");
        // a WHERE literal is canonicalised before comparing, so the uppercase form matches.
        check(e.exec("SELECT id FROM u WHERE uid = '550E8400-E29B-41D4-A716-446655440000'")
                  .rows.size() == 1,
              "WHERE matches case-insensitively");
        // braced form accepted and canonicalised.
        check(e.exec("INSERT INTO u (id, uid) VALUES "
                     "(2, '{6ba7b810-9dad-11d1-80b4-00c04fd430c8}')").ok,
              "insert braced UUID");
        check(cell0(e.exec("SELECT uid FROM u WHERE id = 2")) ==
                  "6ba7b810-9dad-11d1-80b4-00c04fd430c8",
              "braces stripped");
        // teeth: malformed rejected (wrong length / non-hex).
        check(!e.exec("INSERT INTO u (id, uid) VALUES (3, 'not-a-uuid')").ok, "malformed rejected");
        check(!e.exec("INSERT INTO u (id, uid) VALUES (4, '550e8400e29b41d4a71644665544')").ok,
              "short hex rejected");
    }
    // DEFAULT gen_uuid(): deterministic per-table counter, valid v4 shape, distinct per row.
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE g (gid INT, token UUID DEFAULT gen_uuid(), PRIMARY KEY (gid))").ok,
              "create gen_uuid default");
        check(e.exec("INSERT INTO g (gid) VALUES (1)").ok, "insert omitting token (row 1)");
        check(e.exec("INSERT INTO g (gid) VALUES (2)").ok, "insert omitting token (row 2)");
        const std::string t1 = cell0(e.exec("SELECT token FROM g WHERE gid = 1"));
        const std::string t2 = cell0(e.exec("SELECT token FROM g WHERE gid = 2"));
        check(valid_v4_shape(t1), "row 1 token is a valid v4 shape: " + t1);
        check(valid_v4_shape(t2), "row 2 token is a valid v4 shape: " + t2);
        check(t1 != t2, "two generated tokens differ");
        // an explicit value still overrides the default.
        check(e.exec("INSERT INTO g (gid, token) VALUES "
                     "(3, '00000000-0000-4000-8000-000000000000')").ok,
              "explicit token overrides default");
        check(cell0(e.exec("SELECT token FROM g WHERE gid = 3")) ==
                  "00000000-0000-4000-8000-000000000000",
              "explicit token stored");
    }
    // DETERMINISM: the same script on two fresh engines yields the SAME generated token (this is the
    // property the Raft cross-check relies on — generation must be reproducible, never random).
    check(gen_token() == gen_token() && !gen_token().empty(), "gen_uuid is reproducible");

    // gen_uuid() DEFAULT on a non-UUID column is rejected at parse.
    {
        SqlEngine e;
        check(!e.exec("CREATE TABLE bad (id INT DEFAULT gen_uuid(), PRIMARY KEY (id))").ok,
              "gen_uuid on INT column rejected");
    }
    // durable: a client UUID + the gen_uuid() counter survive a restart (no id re-issue / collision).
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        std::string before;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE g (gid INT, token UUID DEFAULT gen_uuid(), PRIMARY KEY (gid))");
            e.exec("INSERT INTO g (gid) VALUES (1)");
            before = cell0(e.exec("SELECT token FROM g WHERE gid = 1"));
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(cell0(rec.exec("SELECT token FROM g WHERE gid = 1")) == before, "recovered token unchanged");
        // a post-restart insert advances PAST the recovered counter — a NEW, distinct token.
        check(rec.exec("INSERT INTO g (gid) VALUES (2)").ok, "insert after recover");
        const std::string after = cell0(rec.exec("SELECT token FROM g WHERE gid = 2"));
        check(valid_v4_shape(after) && after != before, "post-restart token is new + valid");
    }
    if (g_fail) { std::printf("sql_uuid_test: FAILED\n"); return 1; }
    std::printf("sql_uuid_test: OK (UUID over TEXT: canonical, client + deterministic gen_uuid(), "
                "durable)\n");
    return 0;
}
