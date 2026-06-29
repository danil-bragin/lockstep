# SQL feature research — full catalog (pick what to take)

Bar for EVERY taken feature: parser + evaluator (row path; columnar/vectorized fast paths
preserved) + == independent REFERENCE model BYTE-IDENTICAL + TEETH + ASan/UBSan + own commit.
No durability/determinism/verified-core change (pure query layer over the verified txn/store).

## ALREADY PRESENT (do not re-do)
CREATE TABLE/INDEX · DROP INDEX · INSERT (single-row) · UPDATE · DELETE · SELECT · EXPLAIN /
EXPLAIN ANALYZE · projection + `*` + AS aliases + qualified `table.col` · DISTINCT (rows) ·
WHERE: `= != <> < <= > >= AND OR NOT BETWEEN`, `IS [NOT] NULL`, `[NOT] IN (list|subquery)`,
`[NOT] EXISTS (subquery)` · UNCORRELATED subqueries (scalar/IN/EXISTS) · JOIN: INNER + LEFT
[OUTER] (2-table, self-join) · aggregates COUNT/SUM/MIN/MAX/AVG (NULL-skip, INT-trunc) ·
GROUP BY · HAVING · ORDER BY ASC/DESC · LIMIT · OFFSET · types INT + TEXT · single-col PK ·
NOT NULL / nullable + NULL literal · AT STRICT / AT SNAPSHOT N · per-statement txn.

## ABSENT — the menu (id | feature | value | effort | risk | notes)

### A. Expressions & scalars  (A1 is the foundation the rest ride on)
- A1 | scalar arithmetic `+ - * / %` in SELECT/WHERE/HAVING (computed columns, `price*qty AS total`) | HIGH | MED-LARGE | MED | the Expr-tree refactor; MUST preserve vectorized INT-conjunct fast paths as a recognized special case
- A2 | string/scalar fns UPPER LOWER LENGTH SUBSTR TRIM CONCAT COALESCE ABS | MED | MED | LOW | Expr function-call nodes (rides on A1)
- A3 | CASE WHEN … THEN … ELSE … END | HIGH | MED | LOW-MED | conditional Expr (rides on A1)
- A4 | CAST(x AS type) / explicit coercion | LOW-MED | SMALL | LOW | needs >2 types to matter (see F9)

### B. Predicates
- B1 | LIKE / NOT LIKE (`%` `_`) [+ ILIKE] | HIGH | MED-SMALL | LOW | standalone CmpOp + matcher; TEXT → general path, vectorized INT path untouched
- B2 | CORRELATED subqueries (inner sees outer row) | MED | LARGE | MED-HIGH | currently uncorrelated only; per-row re-eval or decorrelation

### C. Aggregates & grouping
- C1 | COUNT(DISTINCT col) / SUM/AVG(DISTINCT) | HIGH | SMALL-MED | LOW | per-group ordered dedup set
- C2 | GROUPING SETS / ROLLUP / CUBE | LOW | MED | LOW | niche OLAP
- C3 | window functions OVER/PARTITION BY (ROW_NUMBER RANK SUM OVER …) | MED-HIGH | LARGE | MED | big; own subproject

### D. Set ops & query composition
- D1 | UNION / UNION ALL | MED-HIGH | MED | LOW-MED | concat (+ ordered dedup for non-ALL); arity/type check
- D2 | INTERSECT / EXCEPT | LOW-MED | MED | LOW | rides on D1 machinery
- D3 | FROM-clause subquery (derived table) `FROM (SELECT …) x` | MED | MED-LARGE | MED | subquery as a row source (today subqueries are predicate-only)
- D4 | CTE `WITH x AS (…)` | MED | MED | LOW-MED | sugar over D3 (non-recursive)
- D5 | INSERT … SELECT | MED | SMALL-MED | LOW | write rows from a query
- D6 | multi-row INSERT `VALUES (…),(…)` | MED | SMALL | LOW | quick win

### E. Joins
- E1 | RIGHT / FULL OUTER JOIN | MED | MED | LOW-MED | explicitly OUT today (only INNER+LEFT)
- E2 | CROSS JOIN | LOW | SMALL | LOW | lexed, not impl'd
- E3 | N-way join (3+ tables) | MED | MED | MED | confirm/extend (2-table proven today)
- E4 | non-equi join / USING clause | LOW-MED | MED | MED | only equi-join ON now

### F. Schema / DDL / types
- F1 | composite (multi-col) PRIMARY KEY + multi-col INDEX | HIGH | MED-LARGE | HIGH | order-preserving composite KEY ENCODING (MVCC/columnar key) — careful, last
- F2 | UNIQUE constraint | MED | MED | MED | enforce on write (read-before-write/index)
- F3 | FOREIGN KEY / REFERENCES | MED | LARGE | MED-HIGH | referential integrity on write/delete
- F4 | DEFAULT column values | MED | SMALL | LOW | fill on INSERT-omit
- F5 | CHECK constraints | LOW-MED | SMALL-MED | LOW | predicate on write (rides on A1)
- F6 | AUTO_INCREMENT / serial PK | MED | SMALL-MED | LOW-MED | server-assigned id allocator
- F7 | ALTER TABLE add/drop/rename column | MED | MED-LARGE | MED | schema evolution + recovered-data reinterpret
- F8 | DROP TABLE | MED | SMALL | LOW | only DROP INDEX today; quick win
- F9 | more types: BIGINT FLOAT/DOUBLE DECIMAL BOOL DATE/TIMESTAMP VARCHAR(n) | MED-HIGH | LARGE | MED | only INT+TEXT now; FLOAT breaks the byte-det INT assumptions — careful

### G. Transactions & misc
- G1 | explicit BEGIN/COMMIT/ROLLBACK (multi-stmt SQL txn) | MED | MED | MED | maps to the txn layer; wire session state
- G2 | UPSERT (INSERT … ON CONFLICT / MERGE) | MED | MED | LOW-MED | read-modify-write on PK
- G3 | NULLS FIRST/LAST in ORDER BY | LOW | SMALL | LOW | null ordering control
- G4 | ORDER BY expression / by aggregate / by output position | LOW-MED | SMALL-MED | LOW | rides on A1

## Suggested execution order IF many are taken (dependency + risk)
quick wins first → D6, F8, C1, B1, G3 · then foundation A1 → A3, A2, F5, G4 ride on it ·
then D1/D2, D5, E1/E2, F4, F6, G2 · then bigger: D3/D4, F9, B2, C3, F2/F3, F7, E3/E4 ·
HIGHEST RISK LAST → F1 (composite key encoding).

## Status (ALL taken — implementing in dependency/risk order)
Quick wins → foundation A1 → riders → mid → big → F1 last.
- [x] D6 multi-row INSERT (atomic; row+columnar == N single inserts; teeth=dup-in-batch) — sql_multirow_insert_test
- [x] F8 DROP TABLE (catalog forget + durable schema tombstone; re-create empty; teeth=unknown) — sql_drop_table_test
- [x] C1 COUNT/SUM/AVG(DISTINCT) (per-group dedup in compute_agg; fast paths gated; teeth) — sql_distinct_agg_test. NOTE: distributed COUNT(DISTINCT) now SHUFFLES (no longer rejected) — see the distributed-SQL section below
- [x] B1 LIKE / NOT LIKE (% _; CmpOp::Like + matcher; vectorizer rejects; NOT LIKE wraps Not; teeth) — sql_like_test
- [x] G3 NULLS FIRST/LAST (order_key_less honors per-key; default=NULL smallest unchanged; teeth) — sql_nulls_order_test
- [x] A1 scalar arithmetic (projection) + A2 string fns + A3 CASE + A4 CAST — scalar Expr engine in SELECT projection (row+columnar, == exact, teeth) — sql_expr_test. NOTE: WHERE-side arithmetic = follow-on
- [x] F5 CHECK (column+table level; source-text captured, persisted, re-parsed+eval per write; atomic; durable) — sql_check_test
- [x] A1/A2/A3/A4 WHERE-side — J1 expression operands in WHERE/CHECK (scalar Expr as a Cmp LHS: `a+b=10`, `doc->>'k'='v'`, `UPPER(name)='BOB'`; row+columnar) — sql_expr_pred_test
- [x] D1 UNION/ALL + D2 INTERSECT/EXCEPT (set-op chain; dedup; combined ORDER/LIMIT; arity teeth) — sql_setops_test
- [x] E2 CROSS — ALREADY PRESENT (see below)
- [x] D3 FROM-subquery (derived tables) · D4 CTE/WITH — materialized into ephemeral tables (chained/joined/aggregated), sql_cte_test
- [x] E4 non-equi join — ALREADY PRESENT (theta via nested_loop; verified)
- [x] F1 composite PRIMARY KEY (all-INT, row-mode): order-preserving concatenated key codec; single-col path byte-identical (conformance/columnar gates green); durable; teeth=TEXT/columnar/dup; UPDATE/DELETE-by-composite + multi-col INDEX = follow-on — sql_composite_pk_test
- [x] E2 CROSS JOIN — ALREADY PRESENT (catalog miss; verified)
- [x] E3 N-way join (3+) — ALREADY PRESENT (catalog miss; verified 3-table)
- [x] D5 INSERT ... SELECT (rows from a query, atomic; arity+dup teeth; both modes) — sql_insert_select_test
- [x] E1 RIGHT/FULL OUTER JOIN (unmatched-right NULL-fill in hash+nested; parser boundary fix for FULL/NULLS; G3 joined ORDER BY completed; teeth) — sql_right_join_test
- [x] F4 DEFAULT (column default fill-on-omit + override + durable catalog; type-mismatch teeth) — sql_default_test
- [x] F6 AUTO_INCREMENT (monotonic id on omit + explicit bump + persisted counter; TEXT teeth) — sql_auto_increment_test
- [x] G2 UPSERT — INSERT ... ON CONFLICT DO NOTHING/UPDATE SET (atomic in batch; index-maintained; PK-update teeth) — sql_upsert_test
- [x] F2 UNIQUE (pre-scan existing + per-batch dedup; NULLs repeat; durable; atomic) — sql_unique_test
- [x] G4 ORDER BY position + computed-column alias (order_key_less resolves position; validation skips it) — sql_orderby_expr_test
- [x] F3 FOREIGN KEY (REFERENCES; insert-time parent-PK check, NULL allowed; DELETE RESTRICT via child scan; durable) — sql_fk_test
- [x] F7 ALTER TABLE ADD COLUMN (row mode; decoders pad ALTER-added suffix cols with DEFAULT/NULL — no rewrite; durable; columnar OUT) — sql_alter_test
- [x] C2 GROUPING SETS (run per set + union; non-set cols NULL; columnar fast-path gated; row+columnar) — sql_grouping_sets_test
- [x] G1 BEGIN/COMMIT/ROLLBACK (write-buffer txn; read-your-writes via read_committed overlay; atomic commit; rollback discards) — sql_txn_test
- [x] B2 correlated subqueries (per-outer-row substitution of outer col refs -> literals; EXISTS/IN/scalar; op-flip on swap) — sql_correlated_test
- [x] C3 window functions (ROW_NUMBER/RANK + SUM/COUNT/MIN/MAX OVER PARTITION BY/ORDER BY; per-partition compute; row+columnar) — sql_window_test
- [x] F9 types (BIGINT/INTEGER/BOOL -> INT, VARCHAR/CHAR -> TEXT, TRUE/FALSE literals; FLOAT/DOUBLE rejected = byte-det) — sql_types_test
- [x] F9e INT128 / DECIMAL128 — 128-bit integer + fixed-point for crypto-scale amounts (wei/sat far beyond int64), as a LOGICAL type over physical TEXT (logical 5/6). Value = 16-byte ORDER-PRESERVING big-endian payload (sign-bit flipped) in Datum.s, so TEXT byte-compare == numeric order and the TEXT key/value codecs are unchanged (byte-deterministic). Keywords INT128/HUGEINT (5), DECIMAL128/NUMERIC128(p,s) (6); DECIMAL(p,s) auto-promotes to 128-bit when p>18 (scale 0..38). coerce parses a decimal string (or widens a bare INT) -> __int128 -> encode_i128; render decodes. Arithmetic: eval_bin_i128 decodes operands (128-bit, or a narrow INT/DECIMAL64, or a bare numeric string with scale inferred) -> checked __int128 +/-/*/÷/unary- (__builtin_*_overflow on __int128) -> re-encode; result INT128 if scale 0 else DECIMAL128. WHERE/ORDER numeric via the order-preserving payload. Literal beyond int128 -> clean reject (parse_i128, not saturating). SUM/AVG over 128-bit done in F9f. Teeth: 1e30 store/render, numeric order vs lexical, add/sub/mul exact, DECIMAL(38,18) 18-digit fraction + 1-wei carry, overflow->error, durable. sql_int128_test; all 51 SQL/conformance/cross green, ASan+UBSan clean
- [x] F13 TIME / ENUM / INTERVAL / JSON — TIME (logical 8, secs-since-midnight, 'HH:MM:SS'), ENUM (logical 9, ordinal over a declared label set on the column, compares by declaration order, render label, decode tags the label), INTERVAL (logical 10, signed seconds; `INTERVAL 'N unit'` literal, week/day/hour/min/sec + HH:MM:SS, months/years rejected) — all INT-backed. Temporal arithmetic in eval_expr (checked int64): TIMESTAMP/TIME ± INTERVAL (TIME wraps; DATE+INTERVAL→TIMESTAMP), INTERVAL±INTERVAL, TS/TIME/DATE diff→INTERVAL, INTERVAL */ number (int128 hook tightened to 5/6). JSON (logical 11 over TEXT) — Json.hpp recursive parser + CANONICAL serialize (sorted keys, compact, normalized numbers, NO float parse → byte-identical equal docs); `->`/`->>` access (object key / 0-based array index; -> yields JSON, ->> text; new Arrow/ArrowText tokens + expr postfix), json_array_length. sql_temporal_enum_test + sql_json_test; all 90 tests pass, ASan+UBSan clean. Follow-on: INTERVAL months/years, JSON @>/path-ops/json_agg, JSON-number→DECIMAL exactness
- [x] F12 ARRAY type — one-dimensional arrays as a logical type (logical=7) over physical TEXT; self-describing payload [elem_logical][elem_scale][count][elements] in Datum.s; byte-deterministic (ordered list of a deterministic element type; FLOAT[] rejected). Element types INT/BIGINT/TEXT/DECIMAL/INT128 (DECIMAL[] keeps scale, INT128[] full width). Column +elem_type/elem_logical/elem_scale (serialized+recovered). render '{a,b,c}'; cmp_datum ELEMENT-WISE then shorter<longer. (a) core: `T[]` type (nested T[][] rejected), ARRAY[...] + '{...}' literals (in expr grammar AND VALUES/WHERE literal reader), `arr[i]` subscript (1-based, out-of-range->NULL), '['/']' tokens, coerce re-encodes per element, vectorized conjunct path bails for arrays. (b) functions: array_length/cardinality, array_contains, array_position, array_append, array_cat; `<scalar-col> <op> ANY|ALL (<array>)` (literal-LHS like `20=ANY(xs)` = follow-on; needs general literal-LHS predicate support). (c) ARRAY_AGG(col) aggregate (scan order, incl NULLs; columnar bails to row-AoS; joins reject = follow-on). (d) UNNEST(arr) in the SELECT list expands one row per element (single UNNEST, not with window fns; UNNEST-in-FROM = follow-on). sql_array_test; all 88 tests pass, ASan+UBSan clean
- [x] F11 follow-ons closed — (1) JOINED SUM/AVG over INT128/DECIMAL128: JoinColumn carries logical/scale; validate_one_agg_joined + compute_agg_joined handle 128-bit (checked __int128). (2) Columnar 128-bit aggregates: confirmed correct via the row-AoS bail + a columnar SUM test (no SIMD rewrite — 16-byte payloads aren't a SIMD win; correctness is the point). (3) WHERE literal too long for VARCHAR(n) is NO MATCH, not an error: coerce gained for_write (default true); the two comparison sites pass false so check_domain skips the length cap (CHAR pad still applies). (4) bare integer literal past int64 is carried as a numeric string (Token.int_overflow + raw digits) -> INT128/DECIMAL128 columns accept it UNQUOTED; an INT64 column gets a clean type error (no silent saturation). (5) BIT(n) (1..63): n-bit UNSIGNED bitmask over INT (reuses int_bits+is_unsigned range check); BIT == BIT(1). sql_int128_test + sql_domain_test extended; all 87 tests pass, ASan+UBSan clean. NOTE: sql_overflow_test updated — a huge bare literal now REJECTS for BIGINT (was: saturate)
- [x] F10 domain constraints — enforce type sizing/limits (was a gap: VARCHAR(n) parsed n but ignored it). One mechanism: check_domain(col, Datum&) at coerce (deterministic; mutates only for CHAR pad). Column gains max_len/fixed_char/is_unsigned/precision/int_bits (all serialized+recovered). (a) VARCHAR(n)/BLOB(n) length cap, CHAR(n) right-pads to n; BLOB/BYTES/BINARY/VARBINARY = TEXT byte aliases. (b) DECIMAL(p,s) precision: reject when |scaled value| has > p digits (int64 + int128 paths). (c) UNSIGNED modifier: reject value < 0 (INT/BIGINT/INT128/DECIMAL/DECIMAL128); a CONSTRAINT, NOT a wider type (storage stays signed; doubles only the positive range for the width-checked int aliases). (d) TINYINT(8)/SMALLINT(16)/INT32/INT4/MEDIUMINT(32) = INT-backed range-validated aliases (signed range, or 0..2^bits-1 with UNSIGNED); INT/INTEGER/BIGINT stay 64-bit unchanged. BIT(n) deferred. Teeth: overflow/too-long/out-of-range/negative all rejected, CHAR pad, durable across restart. sql_domain_test; all 52 SQL/conformance/cross green, ASan+UBSan clean
- [x] F9f SUM / AVG over INT128 / DECIMAL128 — 128-bit aggregation (sum-of-balances, the crypto case). validate_one_agg allows SUM/AVG over logical>=5 (plain TEXT/UUID still rejected); compute_agg accumulates in checked __int128 (AVG truncates toward zero), result keeps the column's logical/scale; MIN/MAX already worked (cmp_datum on the order-preserving payload). The columnar SoA fast path bails to the row-mode AoS (compute_agg) when an aggregate column is 128-bit (the SoA INT folds don't handle the 16-byte payload). GROUP BY + 128-bit SUM works (per-group compute_agg). SUM overflow past int128 -> clean error. JOINED 128-bit SUM = follow-on (validate_one_agg_joined still rejects). sql_int128_test (SUM/AVG/MIN/MAX over INT128 + DECIMAL128, GROUP BY sum, SUM overflow->error)
- [x] F9d checked int64 arithmetic — INT/BIGINT is 64-bit; an op whose true result exceeds int64 was signed-overflow UB (UBSan trap / silent release wrap). Row-mode + joined + window + scalar-expr paths now raise a clean `"integer overflow"` error (`__builtin_*_overflow`): eval_expr +/-/*/÷ (+ unary `-` on INT64_MIN, ÷ INT64_MIN/-1, % INT64_MIN/-1), DECIMAL scale-mul in coerce, compute_agg/compute_agg_joined/compute_window SUM. Opt-in COLUMNAR accumulators use a DEFINED unsigned `wrap_add` (no UB, no branch — SIMD-preserved) since threading an error through the parallel partial-merge machinery would serialize the folds and race; deterministic, UBSan-clean, and conformance never overflows so row==columnar stays byte-identical. Literal beyond int64 still saturates at parse (parse_i64). Teeth: v+v / v*v / SUM overflow → error, normal arithmetic unaffected, huge literal saturates, DECIMAL out-of-range rejected — sql_overflow_test (UBSan: no trap; previously aborted)
- [x] F9c UUID — LOGICAL type over physical TEXT (logical=4). Value = validated, canonicalised (lowercase, dashed) 36-char string; storage/keys/comparison stay TEXT => byte-deterministic. Two sources: client-supplied (coerce validates+canonicalises; braces/uppercase accepted) and DETERMINISTIC `DEFAULT gen_uuid()`/`gen_random_uuid()` — a v4-shaped id (splitmix64 of table_id+counter, version/variant nibbles set) from the per-table next_uuid counter (NOT random — random would diverge across the two Raft impls; persisted+recovered like AUTO_INCREMENT). WHERE literal coerced (case-insensitive match); the conjunct-extraction (vectorized) path now coerces logical-column literals too (also hardened DECIMAL/DATE NOT-NULL WHERE). Teeth: malformed/short rejected, gen_uuid on non-UUID rejected, reproducible, durable. Follow-on: ALTER ADD UUID DEFAULT gen_uuid() (literal-only there) — sql_uuid_test
- [x] F9b DECIMAL(p,s) fixed-point + DATE + TIMESTAMP — LOGICAL types over physical INT (scaled int64 / days-since-epoch / secs-since-epoch). Storage/keys/comparison/ordering stay raw INT => byte-deterministic (FLOAT still OUT). Literal-parse (string '3.14'/'2026-06-27'/'... HH:MM:SS' or whole-number) -> INT via coerce; render() -> logical form (Howard-Hinnant proleptic Gregorian); WHERE literal coerced to the column's INT; DECIMAL +/-/*/÷ fixed-point in eval_expr; SUM/AVG keep scale, MIN/MAX carry the row tag; schema logical/scale persisted+recovered. Follow-on: columnar/joined/access-path literal coercion (row-mode done) — sql_decimal_date_test

## J-series + later follow-ons (all DONE; were "follow-on" above)
- [x] J1 expression operands in WHERE/CHECK (scalar Expr LHS of a comparison) — sql_expr_pred_test
- [x] J2 expression / JSON-path index `((expr))` (functional index over a computed key) — sql_expr_index_test
- [x] J3 array-element GIN index `USING GIN` (membership over array columns) — sql_array_gin_test
- [x] DROP CONSTRAINT by name (drop FK/UNIQUE/CHECK by name; NamedConstraint registry) — sql_drop_constraint_test
- [x] JSON `@>` containment + `#>`/`#>>` path ops + `json_agg` — sql_json_ops_test
- [x] INTERVAL months/years (logical month-interval, 12-month logical year; calendar add) — sql_interval_month_test
- [x] UINT256 (logical 13; 32-byte big-endian payload; crypto-scale beyond 128-bit) — sql_uint256_test
- [x] secondary / composite / partial / merge indexes + ANALYZE stats + cost-based planning — sql_index_test, sql_analyze_test
- [x] D3 FROM-subquery (derived tables) · D4 CTE / `WITH` — DONE (sql_cte_test). Single-node SQL surface now complete.

## Distributed SQL (DistributedSql.hpp — scatter/gather over M shards)
A coordinator fans a statement across shards and merges results byte-identically to a
single node holding all rows (gate: tests/sql_distributed_test.cpp). DDL broadcasts;
INSERT/UPDATE/DELETE/point-SELECT route by PK hash; scan/aggregate scatter + merge.
- [x] scatter-gather aggregates + GROUP BY merge (COUNT/SUM/MIN/MAX recombine; re-sorted)
- [x] co-located-shuffle STAR-JOIN pushdown — fact aggregated by the join key ON EACH SHARD,
      per-key partials merged, tiny dim gathered + rolled up; the large fact is NEVER gathered
- [x] WHERE pushdown — split a pure-AND filter into fact-side (per-shard) + dim-side (dim gather)
- [x] AVG pushdown — SUM/COUNT split, divided at the coordinator
- [x] HAVING — coordinator post-filter on rolled-up groups (+ scatter-path single-table HAVING fix)
- [x] COUNT(DISTINCT) global shuffle — ship distinct (group,value) tuples, dedup at coordinator
- [x] multi-dimension star (1 fact + N dims; composite join key; snowflake chains → gather)
- [x] broadcast-dim join — a replicated dim (set_replicated → writes broadcast) joined per-shard,
      partial group aggregates merged (better when the join key has ≫ more values than #groups)
- [ ] open: SUM/AVG(DISTINCT) shuffle (only COUNT shuffles now) · true snowflake (dim→dim) ·
      projection broadcast-join + global ORDER BY merge-sort · cost-based pushdown-vs-broadcast choice
