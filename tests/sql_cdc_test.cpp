// sql_cdc_test.cpp — K4: CDC changefeeds. CHANGES t SINCE s [LIMIT n] over the committed
// op-log: globally Seq-ordered, PUT carries the full row image, DELETE the PK.
// THE GATE: replaying the changefeed from 0 into a shadow map (idempotent upsert/delete)
// reproduces the table EXACTLY — and a resumed cursor (split consumption) reproduces the
// same shadow, proving Seq is an exactly-once resume token by construction.
#include <cstdio>
#include <map>
#include <utility>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/DistributedSql.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
// Apply one CHANGES result to a shadow (pk -> rendered non-pk cols); returns last _seq.
std::int64_t apply_feed(const ExecResult& r, std::map<std::int64_t, std::string>& shadow) {
    std::int64_t last = 0;
    for (const auto& row : r.rows) {
        last = row.cells[0].second.i;
        const std::string op = row.cells[1].second.s;
        const std::int64_t pk = row.cells[2].second.i;
        if (op == "DELETE") {
            shadow.erase(pk);
        } else {
            std::string img;
            for (std::size_t c = 3; c < row.cells.size(); ++c)
                img += row.cells[c].second.render() + "|";
            shadow[pk] = img;
        }
    }
    return last;
}
std::map<std::int64_t, std::string> table_image(SqlEngine& e) {
    std::map<std::int64_t, std::string> img;
    const ExecResult r = e.exec("SELECT id, name, score FROM t ORDER BY id");
    for (const auto& row : r.rows) {
        img[row.cells[0].second.i] =
            row.cells[1].second.render() + "|" + row.cells[2].second.render() + "|";
    }
    return img;
}
}  // namespace

int main() {
    std::printf("=== sql_cdc_test (K4 changefeeds) ===\n");
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id,name,score) VALUES (1,'a',10), (2,'b',20), (3,'c',30)");
    e.exec("UPDATE t SET score = 25 WHERE id = 2");
    e.exec("DELETE FROM t WHERE id = 1");
    e.exec("INSERT INTO t (id,name,score) VALUES (4,'d',40)");

    // (1) THE REPLAY GATE: consume from 0, apply, compare against the live table.
    std::map<std::int64_t, std::string> shadow;
    const ExecResult all = e.exec("CHANGES t SINCE 0");
    check(all.ok && !all.rows.empty(), "CHANGES t SINCE 0 returns ops");
    const std::int64_t last = apply_feed(all, shadow);
    check(shadow == table_image(e), "replayed changefeed == live table");
    check(last > 0, "last _seq is a usable cursor");

    // (2) Ordering: _seq strictly ascending (the GLOBAL total order).
    bool asc = true;
    for (std::size_t i = 1; i < all.rows.size(); ++i)
        asc = asc && all.rows[i].cells[0].second.i > all.rows[i - 1].cells[0].second.i;
    check(asc, "_seq strictly ascending");

    // (3) Exactly-once resume: split consumption at an arbitrary cursor == one pass.
    {
        std::map<std::int64_t, std::string> s2;
        const ExecResult first = e.exec("CHANGES t SINCE 0 LIMIT 3");
        const std::int64_t cur = apply_feed(first, s2);
        const ExecResult rest = e.exec("CHANGES t SINCE " + std::to_string(cur));
        apply_feed(rest, s2);
        check(s2 == shadow, "split cursor consumption == single pass (exactly-once resume)");
    }

    // (4) Incremental tail: new writes appear after the cursor; old ones do not repeat.
    e.exec("INSERT INTO t (id,name,score) VALUES (5,'e',50)");
    e.exec("DELETE FROM t WHERE id = 3");
    const ExecResult tail = e.exec("CHANGES t SINCE " + std::to_string(last));
    check(tail.ok && tail.rows.size() == 2, "tail has exactly the two new ops");
    apply_feed(tail, shadow);
    check(shadow == table_image(e), "shadow tracks the table incrementally");

    // (5) Filtering: another table's writes never leak into this feed.
    e.exec("CREATE TABLE other (id INT, x INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO other (id,x) VALUES (1,1)");
    const ExecResult t2 = e.exec("CHANGES t SINCE " + std::to_string(last));
    check(t2.rows.size() == 2, "other-table writes filtered out");

    // (6) DELETE row shape: PK present, other columns NULL.
    bool del_ok = false;
    for (const auto& row : tail.rows)
        if (row.cells[1].second.s == "DELETE")
            del_ok = row.cells[2].second.i == 3 && row.cells[3].second.is_null &&
                     row.cells[4].second.is_null;
    check(del_ok, "DELETE carries the PK, NULL elsewhere");

    // (7) Restart durability: WAL replay rebuilds the op-log source — the changefeed
    // (and the resume cursor) survive a crash/restart with the SAME seqs.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x4Cull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        std::vector<std::pair<std::int64_t, std::string>> before;
        {
            SqlEngine d(sched, data, sched, cat);
            d.exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
            d.exec("INSERT INTO t (id,name,score) VALUES (1,'a',10), (2,'b',20)");
            d.exec("DELETE FROM t WHERE id = 1");
            for (const auto& row : d.exec("CHANGES t SINCE 0").rows)
                before.emplace_back(row.cells[0].second.i, row.cells[1].second.s);
        }
        {
            SqlEngine d(sched, data, sched, cat);
            d.recover(data.logical_len(), cat.logical_len());
            std::vector<std::pair<std::int64_t, std::string>> after;
            for (const auto& row : d.exec("CHANGES t SINCE 0").rows)
                after.emplace_back(row.cells[0].second.i, row.cells[1].second.s);
            check(!before.empty() && before == after,
                  "changefeed byte-stable across restart (same seqs, same ops)");
        }
    }

    // (8) K4.2 per-shard feeds — the Kafka-partition shape. M shards = M independent
    // Seq lines; one cursor per shard. THE GATE: the union of all per-shard replays
    // == the whole distributed table, each shard's feed internally Seq-ordered, and a
    // per-shard split cursor resumes exactly-once — while cross-shard requires SHARD
    // (a global cross-shard seq does not exist, same as across Kafka partitions).
    {
        constexpr std::size_t kShards = 4;
        std::vector<SqlEngine> shards(kShards);
        std::vector<EngineSqlShard> wraps;
        wraps.reserve(kShards);
        for (SqlEngine& s : shards) wraps.emplace_back(&s);
        std::vector<ISqlShard*> ptrs;
        for (EngineSqlShard& w : wraps) ptrs.push_back(&w);
        DistributedSql dist(ptrs);
        dist.exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
        for (int i = 0; i < 40; ++i)
            dist.exec("INSERT INTO t (id,name,score) VALUES (" + std::to_string(i) + ",'u" +
                      std::to_string(i) + "'," + std::to_string(i * 3) + ")");
        dist.exec("DELETE FROM t WHERE id = 7");
        dist.exec("UPDATE t SET score = 999 WHERE id = 11");

        std::map<std::int64_t, std::string> uni;
        bool per_shard_ok = true;
        for (std::size_t sh = 0; sh < dist.shard_count(); ++sh) {
            // Split consumption per shard: LIMIT 4 then resume from the cursor.
            std::map<std::int64_t, std::string> a, b;
            const ExecResult full =
                dist.exec("CHANGES t SHARD " + std::to_string(sh) + " SINCE 0");
            per_shard_ok = per_shard_ok && full.ok;
            apply_feed(full, a);
            const ExecResult head =
                dist.exec("CHANGES t SHARD " + std::to_string(sh) + " SINCE 0 LIMIT 4");
            const std::int64_t cur = apply_feed(head, b);
            apply_feed(dist.exec("CHANGES t SHARD " + std::to_string(sh) + " SINCE " +
                                 std::to_string(cur)),
                       b);
            per_shard_ok = per_shard_ok && a == b;
            for (const auto& [k, v] : a) uni[k] = v;
        }
        check(per_shard_ok, "each shard feed replays + resumes exactly-once");
        std::map<std::int64_t, std::string> want;
        const ExecResult sel = dist.exec("SELECT id, name, score FROM t ORDER BY id");
        for (const auto& row : sel.rows)
            want[row.cells[0].second.i] =
                row.cells[1].second.render() + "|" + row.cells[2].second.render() + "|";
        check(uni == want && want.size() == 39,
              "union of per-shard replays == whole distributed table");
        check(!dist.exec("CHANGES t SINCE 0").ok, "cross-shard CHANGES demands SHARD");
        check(!dist.exec("CHANGES t SHARD 4 SINCE 0").ok, "shard index out of range rejected");
        check(!dist.exec("CREATE CHANGEFEED cf FOR t").ok,
              "named feeds are per-shard (coordinator refuses)");
    }
    check(!e.exec("CHANGES t SHARD 0 SINCE 0").ok, "SHARD on a single engine rejected");

    // (9) Retention knob surface: SET cdc.retain_seq wires through to the engine.
    check(e.exec("SET cdc.retain_seq = 1").ok, "SET cdc.retain_seq accepted");
    check(!e.exec("SET cdc.retain_seq = 'x'").ok, "non-integer retain_seq rejected");

    // (10a) Columnar tooth: a columnar table refuses CHANGES with a teaching error
    // (its delta->block maintenance would surface as false DELETEs) — never a
    // silently empty feed.
    {
        SqlEngine d;
        d.set_columnar_default(true);
        check(d.exec("CREATE TABLE ct (id INT, v INT, PRIMARY KEY (id))").ok, "columnar create");
        d.set_columnar_default(false);
        const ExecResult r = d.exec("CHANGES ct SINCE 0");
        check(!r.ok && r.error.find("columnar") != std::string::npos,
              "columnar CHANGES: clean teaching refusal");
    }

    // (10) Teeth: unknown table; LIMIT respected.
    check(!e.exec("CHANGES nosuch SINCE 0").ok, "unknown table rejected");
    check(e.exec("CHANGES t SINCE 0 LIMIT 2").rows.size() == 2, "LIMIT respected");

    // (11) K4.4 named changefeeds: server-side durable cursors + auto-retention.
    {
        SqlEngine d;
        d.exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
        d.exec("INSERT INTO t (id,name,score) VALUES (1,'a',10), (2,'b',20), (3,'c',30)");
        check(d.exec("CREATE CHANGEFEED cf FOR t").ok, "CREATE CHANGEFEED");
        check(!d.exec("CREATE CHANGEFEED cf FOR t").ok, "duplicate feed rejected");
        check(!d.exec("CREATE CHANGEFEED nope FOR missing").ok, "feed on unknown table rejected");

        const ExecResult f1 = d.exec("FETCH cf");
        const ExecResult direct = d.exec("CHANGES t SINCE 0");
        check(f1.ok && f1.rows.size() == direct.rows.size() && f1.rows.size() == 3,
              "FETCH == CHANGES SINCE 0 (fresh cursor)");
        const ExecResult f2 = d.exec("FETCH cf");
        check(f2.ok && f2.rows.size() == 3, "FETCH does NOT advance (crash-safe refetch)");

        const std::int64_t last = f1.rows.back().cells[0].second.i;
        check(d.exec("ACK CHANGEFEED cf AT " + std::to_string(last)).ok, "ACK advances");
        check(d.exec("FETCH cf").rows.empty(), "post-ACK FETCH is empty");
        check(!d.exec("ACK CHANGEFEED cf AT 1").ok, "cursor never moves backwards");
        d.exec("INSERT INTO t (id,name,score) VALUES (4,'d',40)");
        const ExecResult f3 = d.exec("FETCH cf");
        check(f3.ok && f3.rows.size() == 1 && f3.rows[0].cells[2].second.i == 4,
              "FETCH returns only the new tail");

        // Auto-retention: the slowest feed's cursor + 1 becomes the horizon.
        check(d.exec("CREATE CHANGEFEED cf2 FOR t").ok, "second feed");
        d.exec("ACK CHANGEFEED cf2 AT 2");
        check(d.cdc_retain_seq() == 3, "horizon = min(acked)+1 (slow feed cf2 at 2)");
        check(d.exec("DROP CHANGEFEED cf2").ok, "DROP CHANGEFEED");
        check(d.cdc_retain_seq() == last + 1, "horizon recomputed after DROP (cf's ack)");
        check(!d.exec("FETCH cf2").ok, "dropped feed gone");
        check(!d.exec("FETCH ghost").ok && !d.exec("ACK CHANGEFEED ghost AT 5").ok,
              "unknown feed teeth");
    }

    // (12) Named cursor survives a restart (the registry is an ordinary durable table).
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x4Dull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        std::int64_t acked_at = 0;
        {
            SqlEngine d(sched, data, sched, cat);
            d.exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
            d.exec("INSERT INTO t (id,name,score) VALUES (1,'a',10), (2,'b',20)");
            d.exec("CREATE CHANGEFEED cf FOR t");
            const ExecResult f = d.exec("FETCH cf");
            acked_at = f.rows[0].cells[0].second.i;  // ack only the FIRST op
            d.exec("ACK CHANGEFEED cf AT " + std::to_string(acked_at));
        }
        {
            SqlEngine d(sched, data, sched, cat);
            d.recover(data.logical_len(), cat.logical_len());
            const ExecResult f = d.exec("FETCH cf");
            check(f.ok && f.rows.size() == 1 && f.rows[0].cells[0].second.i > acked_at,
                  "cursor position survives restart (exactly the unacked tail)");
        }
    }

    // (13) K4.10 topics: append-only log — publish/consume, txn outbox, restart.
    {
        SqlEngine d;
        check(d.exec("CREATE TOPIC ev").ok, "CREATE TOPIC");
        check(!d.exec("CREATE TOPIC ev").ok, "duplicate topic rejected");
        check(!d.exec("PUBLISH ghost, 'x'").ok, "unknown topic rejected");
        for (int i = 0; i < 5; ++i)
            check(d.exec("PUBLISH ev, 'm" + std::to_string(i) + "'").ok, "PUBLISH");
        const ExecResult c = d.exec("CONSUME ev SINCE 0");
        check(c.ok && c.rows.size() == 5 && c.rows[0].cells[1].second.s == "m0" &&
                  c.rows[4].cells[1].second.s == "m4",
              "CONSUME returns all, FIFO by offset");
        // Cursor discipline: resume from last offset + 1.
        const std::int64_t last = c.rows[2].cells[0].second.i;
        const ExecResult rest = d.exec("CONSUME ev SINCE " + std::to_string(last + 1));
        check(rest.ok && rest.rows.size() == 2, "offset cursor resume");
        check(d.exec("CONSUME ev SINCE 0 LIMIT 2").rows.size() == 2, "LIMIT");
        // Transactional outbox: rollback publishes vanish; offsets stay dense.
        d.exec("BEGIN");
        d.exec("PUBLISH ev, 'doomed'");
        d.exec("ROLLBACK");
        check(d.exec("CONSUME ev SINCE 5").rows.empty(), "rolled-back publish absent");
        check(d.exec("PUBLISH ev, 'm5'").rows[0].cells[0].second.i == 5,
              "offsets dense after rollback");
    }
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x4Eull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        {
            SqlEngine d(sched, data, sched, cat);
            d.exec("CREATE TOPIC ev");
            d.exec("PUBLISH ev, 'a'");
            d.exec("PUBLISH ev, 'b'");
        }
        {
            SqlEngine d(sched, data, sched, cat);
            d.recover(data.logical_len(), cat.logical_len());
            check(d.exec("CONSUME ev SINCE 0").rows.size() == 2, "topic survives restart");
            const ExecResult p = d.exec("PUBLISH ev, 'c'");
            check(p.ok && p.rows[0].cells[0].second.i == 2,
                  "tail counter rebuilt from storage after restart");
        }
    }

    if (g_fail != 0) { std::printf("sql_cdc_test: FAILURES\n"); return 1; }
    std::printf("sql_cdc_test: ALL PASS\n");
    return 0;
}
