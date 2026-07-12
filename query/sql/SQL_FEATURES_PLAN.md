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
- [x] H1 CREATE [OR REPLACE] VIEW [(cols)] AS SELECT · DROP VIEW [IF EXISTS] — a view is a NAMED, stored SELECT kept as its RAW source text (re-parsed on every reference, so durable + AST-version-independent); a FROM reference expands to the view's query, reusing the D3/D4 derived-table materialization (nested views expand recursively; a self-referential view errors, no loop). Views share the table NAMESPACE (a name is a table OR a view; DDL enforces the exclusion, but an ephemeral materialization deliberately shadows the view with a same-named transient table). Optional explicit column list renames the output columns; CREATE OR REPLACE overwrites, IF NOT EXISTS is a no-op. Durable via a dedicated catalog namespace (0x04 view records, tombstoned on DROP; recovered in reprime_catalog_from_store). Distributed: DDL broadcasts to every shard. Teeth: basic/aggregate/join/rename/nested/replace/drop, unknown-view DROP errors, table<->view name clash refused both ways, recursive view errors, survives restart. sql_view_test; all 125 tests green, conformance byte-identical, ASan+UBSan clean.

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

## K10 + W3 + W9 (2026-07-05 session)

- [x] **K10 time travel** — `SELECT ... AS OF SEQ n` (alias for AT SNAPSHOT n; keyword SEQ/SNAPSHOT/VERSION optional). MVCC read-as-of; backed by existing snapshots + PITR. `sql_time_travel_test`.
- [x] **W3.1 query memory governance** — deterministic per-statement byte cap (off by default): `set_max_query_memory()` C++ + `SET lockstep.max_query_memory = N` SQL; charged at intermediate materialization (derived/CTE/view) AND the returned result set; deterministic "query memory limit exceeded" error, byte-identical across replicas. `sql_query_memory_test`.
- [x] **W9 information_schema** — tables · columns · views · schemata · table_constraints · key_column_usage. Synthesised on the fly from the live catalog (materialize_typed → ephemeral table → normal SELECT path, so WHERE/projection/joins work; a real table wins).
- [x] **W9 pg_catalog** — pg_tables (+ `pg_tables` alias) · pg_namespace · pg_class (relname/relkind r/v). Plain SELECTs work; full psql `\d` (oids/joins) is a later step. `sql_information_schema_test`.
- [x] **W9 session functions** — version() (PG-compat string) · current_database/current_catalog/current_schema/current_user/session_user/user(). `sql_connection_funcs_test`.
- [x] **W9 scalar functions** — NULLIF · GREATEST · LEAST · MOD · SIGN · REVERSE · REPEAT · LEFT · RIGHT · LTRIM · RTRIM · STRPOS/POSITION. `sql_scalar_funcs_test`.
- [ ] open (W9): sequences (CREATE SEQUENCE/nextval — needs a deterministic-replication design) · COPY FROM/TO · prepared-statement plan cache · pg_catalog oids for real `\d` · table_constraints for CHECK · cursors.
- [ ] open (W5): cost-based JOIN reorder + hash-join build-side swap (stats via ANALYZE already exist; swap changes join output-column layout — needs care).

## Batch 2026-07-06 (session cont'd) — aggregates, window fns, matviews, governance

- [x] **aggregates**: STRING_AGG(col, delim) / GROUP_CONCAT · BOOL_AND · BOOL_OR · BIT_AND · BIT_OR (non-fusable, row-AoS).
- [x] **window functions**: DENSE_RANK · LAG(col) · LEAD(col) · FIRST_VALUE(col) · LAST_VALUE(col) · NTILE(n) · AVG(col) OVER — complementing ROW_NUMBER/RANK/SUM/COUNT/MIN/MAX OVER.
- [x] **date/time**: DATE_PART('field', date|ts) · EXTRACT(field FROM ...) · DATE_TRUNC('unit', ...) (year..second, dow/doy/epoch).
- [x] **scalar**: NULLIF · GREATEST · LEAST · MOD · SIGN · REVERSE · REPEAT · LEFT · RIGHT · LTRIM · RTRIM · STRPOS/POSITION · SPLIT_PART · INITCAP · LPAD · RPAD · ASCII · CHR · version()/current_database()/current_schema()/current_user()/session_user().
- [x] **K5 materialized views**: CREATE/REFRESH/DROP MATERIALIZED VIEW (non-incremental; backing table + source durable).
- [x] **W3 governance**: SET lockstep.max_query_memory · per-statement mem cap (materialize + JOIN) · cancellation seam · AS OF SEQ time travel (K10) · parse cache.
- [x] **information_schema / pg_catalog**: tables/columns/views/schemata/table_constraints/key_column_usage/constraint_column_usage + pg_tables/pg_namespace/pg_class.
- [ ] open: incremental IVM (auto-refresh matview on base change) · window frames (ROWS/RANGE BETWEEN) · RECURSIVE CTE · COPY · CREATE SEQUENCE (deterministic-replication design) · off-reactor concurrency.

## K1 vector search (2026-07-07 session) — VECTOR type, pgvector operators, IVFFLAT

- [x] **F14 groundwork (prev commits)**: REAL/DOUBLE PRECISION type · bare float literals · REAL arithmetic + aggregates.
- [x] **K1 exact k-NN (prev commit)**: L2_DISTANCE/EUCLIDEAN_DISTANCE · COSINE_DISTANCE · INNER_PRODUCT/DOT_PRODUCT over REAL[]/INT[] + brute-force ORDER BY fn LIMIT k; columnar read_block_row logical-tag fix.
- [x] **K1.1 VECTOR(n) type** — logical 15 over the ARRAY codec (REAL elements); dim in Column::max_len, enforced at coerce; pgvector '[x,y,z]' text literal + render; ARRAY[...]/'{...}' accepted; DESCRIBE VECTOR(n); information_schema 'vector' (+ REAL now 'double precision'); dim 1..16000; VECTOR[] rejected. `sql_vector_test`.
- [x] **K1.2 distance operators** — `<->` (L2) · `<#>` (NEGATIVE inner product, pgvector) · `<=>` (cosine); lowest scalar precedence; SELECT/WHERE/ORDER BY. **ORDER BY <scalar-expr>** (new, general): hidden-cell sort on the single-table path (eval per source row → sort → strip), SELECT * ok; DISTINCT/UNNEST/agg/join paths fail closed; lexer keeps `<=`/`<>`/`a < -5` byte-identical.
- [x] **K1.3 USING IVFFLAT** — `WITH (lists = N, probes = M)`; deterministic k-means (PK-ordered even seeding, 5 fixed Lloyd iterations, low-index tie-break — no rng, replica-identical centroids frozen in the catalog); entry = [list_id ++ pk] -> vector payload (probe needs no row fetch); INSERT/UPDATE/DELETE maintain in-txn; ANN path for `ORDER BY emb <-> const LIMIT k` (no WHERE), (distance, PK-bytes) total order == exact path's tie-break; probes=lists == brute force EXACTLY (differential gate); planner never uses ivfflat for eq/range/merge/covering; durable (recovered centroids bucket fresh INSERTs).
- [ ] open (K1): pgvector BINARY wire format + vector OID on lockstep_pgd (text format works) · SIMD distance kernels · `SET ivfflat.probes` session knob · K1.4 deterministic HNSW (seeded levels + replica index-hash check) · K1.5 recall/latency bench vs pgvector @1M×768d · cosine/inner-product ivfflat opclasses (L2 only today).

## K1.3b-d follow-ons (2026-07-08)

- [x] **opclasses**: vector_l2_ops (default) · vector_cosine_ops · vector_ip_ops — cosine/IP cluster+assign in normalized direction space, candidates ranked by the opclass's exact distance (IEEE-identical to the scalar kernel → probes=lists differential gate holds per opclass); ANN matcher pairs operator↔opclass, mismatch = exact fallback.
- [x] **SET ivfflat.probes = n** — session override of the per-index probes default (0 restores); pgvector's recall knob.
- [x] **honest EXPLAIN** — same ivfflat_match as the executor: `Limit -> Ivfflat Scan: using <ix> (opclass, lists=N, probes=M)`; ANALYZE actual = entries examined.
- [x] **micro-bench (K1.5-lite, host -O2)**: 20k×32d, k=10, lists=100, probes=4 → **7.3x** vs brute-force (7.3 ms/query vs 53.5), recall@10 = **0.958**, build 260 ms. Full 1M×768d vs real pgvector still open.

## K1.4 deterministic HNSW (2026-07-11)

- [x] **USING HNSW WITH (m, ef_construction)** — levels = integer-geometric hash of PK bytes (no rng, no libm → bit-stable across replicas); graph in index-KV ('v' vector+flags, 'n' per-level adjacency, 'e' entry); maintenance in the SAME atomic batch, overlay reads (multi-row INSERT links within itself); DELETE = zombie (traversable, excluded); UPDATE = same-level relink; `SET hnsw.ef_search`; honest EXPLAIN `Hnsw Scan`; AT SNAPSHOT works; ef>=N == exact gate (L2 + cosine) row+columnar+durable.
- **HONEST BENCH (host -O2, 5k×32d, k=10)**: build 261 s (~52 ms/insert), query 84 ms (0.1x vs brute 12.4 ms), recall@10 0.826 — **correct + replica-attestable but NOT yet fast**: each graph hop = a point KV get through the full Query machinery (~130 µs × ~640 visited nodes). ivfflat reads probe lists as RANGE scans → 0.84 ms/query, 14.7x, recall 0.958 at the same scale. **Recommendation today: ivfflat.** HNSW win needs storage batch-get / cheap point-read path (ties into the ~17k ops/s single-thread coroutine ceiling) — future work.
- [ ] open: batch/cached graph reads (or inline neighbor vectors + reverse-edge maintenance) · REINDEX/vacuum to compact zombies · replica graph-hash attestation demo over the keyspace-hash machinery.

## K1 wire + portability (2026-07-11, cont'd)

- [x] **PG wire vector**: vector OID 16388 in RowDescription (+ REAL→float8, JSON→json, UUID→uuid) · NEW pg_catalog.pg_type (client `SELECT oid FROM pg_type WHERE typname='vector'` works — psycopg register_vector path) · Bind format codes HONORED: binary int2/4/8, float4/8, bool, text, pgvector vector binary ([dim u16][unused u16][float4 BE]) decoded via Parse-time OIDs, both PgSession + ProdPgRaftServer; undecodable binary param / binary RESULT request = clean 0A000 (never garbage).
- [x] **parse_double_strict** — libc++ 18 (Docker/TSan toolchain) lacks FP from_chars; whole F14/K1 layer didn't compile there (CI green = libstdc++ only). Shared helper: from_chars if available else strtod_l("C"), from_chars-grammar-tightened. Docker-Linux libc++ Release now builds + tests green.
- **SIMD kernels: deliberately SKIPPED** — manual/auto FP vectorization changes accumulation order → distance values differ per platform lane-width → breaks cross-replica byte-identity + kernel-identity gates. Determinism > throughput; revisit only as a fixed-width (always-4-lane) portable kernel applied to BOTH exact and index paths at once.
- [ ] open: binary RESULT format (text-only today) · K1.5 bench vs real pgvector · HNSW batch-get perf · REINDEX.

## K1.5 bench harness vs real pgvector (2026-07-11) — PRELIMINARY, found a scaling cliff

- [x] harness `bench/compare/vector/` (run_vector.sh + lockstep_vector.cpp + REPORT.md): identical deterministic data both sides, cpu-pinned, Docker pgvector:pg16.
- **FINDING (blocks a quotable report): Lockstep k-NN does not scale past ~50k rows** — brute ms/query 12.5k→311, 25k→774, 50k→1553 (≈linear), 100k→26242 (**17x cliff**; suspect row-materialization memory blow-up → swap, NOT the distance kernel). pgvector at 100k×64: brute 55 ms, ivfflat 1.77 ms, build 1.9 s — the target to close.
- recall comparison invalid on the current dataset (100 tight clusters → tie-dominated top-k; our 1.0 = determinism artifact, pgvector 0.09 = tie-shuffle + k-means degeneracy). Need separated points.
- [ ] next: profile-first the 50k→100k cliff · dataset v2 (separated points) · rerun · publish.

## scan cliff fixed (2026-07-11, 4e68dc3) + K1.5 run 2

- [x] **storage scan_task O(N^2) post-flush cliff KILLED** (profile-first: 92% samples in vector::insert; SSTable runs now fold via linear 2-way merge, offer-identical winner rule). Brute k-NN 100k: 26242→415 ms host (63x); linear to 200k. Every flushed-table reader wins.
- **K1.5 run 2 @100k×64 (docker, 1 cpu)**: brute 597 vs 13.2 ms (45x), ivfflat 156 vs 0.69 ms (226x), build 4.5 vs 0.4 s. Gap causes known: AST-interpreted distance + full-width row materialisation (brute); per-candidate Datum decode via ARRAY codec (probe). Next rungs: raw-double probe scoring · narrow materialisation for the k-NN shape · dataset v2 for recall.

## K1 perf rungs 1-2 (2026-07-11, abea55e)

- [x] raw-double scoring (ivf_doubles_fast byte-walk, no Datum per element) + const-query-vector memo (vec_lit_cache_) + narrow ORDER-BY-expr materialisation (expr_mark_cols). @100k×64 docker: brute 597→145 ms (4.1x), ivfflat 156→80 ms (2x). **Gap to pgvector: brute 11x, indexed 116x** (was 45x/226x). Bit-identical (bench differential gate holds).
- [ ] next rungs: probe residual = Query range-scan machinery per list + candidate sort — batch the probed lists into one scan? · float4 payload storage option · dataset v2 recall · HNSW batch-get.

## K1 perf rung 3 (2026-07-11)

- [x] fused probe scoring (ivf_score_fast off raw bytes) + winner-only pk decode (Cand = dist+pk-bytes, suffix substr) + partial_sort(k+offset). ivfflat @100k×64: host 41→21.7 ms, docker 156→62 ms — **gap to pgvector ~92x indexed / 11x brute** (morning: 226x/45x). Residual = per-list Query scan machinery (scan_into/offer/memcmp ≈ dominant in profile) — next seam is storage-side batch scan or probe-list layout.

## K1 perf rung 4 (2026-07-11, 7b34192) — scan_visit seam, honest no-win

- [x] storage::Engine::scan_visit (sync, zero-materialisation, vlog-declines) + WalEngine build_scan_merged extraction; IVFFLAT probe scores inside the visit at Strict level. **No wall-clock win (~22ms unchanged)** — skipped layers (Task/Promise/out-vector/Query) were cheap; profile floor = merge itself (SSTableReader::scan_into copies key+value per entry, ~4µs/entry).
- [ ] NEXT RUNG: streaming SSTable cursor (iterator scan_into) plugged into build_scan_merged — kills the per-entry acc copies; then revisit vs pgvector 0.67ms.

## K1 perf rung 5 (2026-07-11) — SSTable scan seek

- [x] scan_into seeks the sparse index (lookup-style back-up, break past hi) — was O(table) per range scan on flushed data. ivfflat @100k×64 host 21.7→16 ms; benefits EVERY narrow range read (index lookups, GIN, columnar families, PK ranges). Output byte-identical.
- [ ] remaining probe floor: accepted-entry key+value copies in acc/run + merge — streaming per-source cursors into build_scan_merged if further cuts needed; vs pgvector 0.67 ms the honest gap is now ~65x host-measured.

## K1 perf rung 6 (2026-07-11, a5352bd) — streaming zero-copy scan_visit

- [x] scan_visit = streaming k-way cursor merge (memtable iter + SSTableReader block cursors, offer-identical winner rule, pointer-buffer then callbacks, vlog falls back pre-callback). ivfflat @100k×64 host 16→10.6 ms. **Day total indexed: 4267→10.6 (~400x)**; vs pgvector 0.67 ms ≈ 16x (host-vs-docker rough).
- [ ] next: clean profile of the 10.6 ms residual (last sample mixed the brute phase) — suspects: per-candidate pk_bytes substr alloc + push_back · ivf byte-assembly (LE memcpy fast path) · std::function indirection · per-list overhead · then the 4-accumulator kernel (both paths together) if the math surfaces.

## K1 perf rungs 7-8 (2026-07-11) — verdict: probe is MEMORY-BOUND

- [x] rung 7 (1dc5d5b): fixed-stride LE memcpy scorer/decoder — flat.
- [x] rung 8 (c003e6b): ONE shared 4-lane kernel (vec_accum4/vec_finish, fixed lanes+combine, bit-deterministic per host) in exact+probe+HNSW — flat (~11.2-11.6 ms @100k×64).
- **Diagnosis: memory-bound** — ~5000 scattered ~600B double payloads (~3MB irregular reads/query) vs pgvector's contiguous float4 pages. Compute levers exhausted.
- [ ] **NEXT (the parity move): per-list contiguous float32 probe cache + exact double re-rank of the top window** — prune in f32 over dense blocks (~1/2 bytes, prefetch-friendly), final ordering bit-exact via the shared kernel on the re-rank window. Cache = derived data (rebuildable, non-replicated layout, deterministic content).

## K1 perf rung 9 (2026-07-11, 215d23a) — contiguous probe cache: 6.6x, gap 3.5x

- [x] per-list dense-double probe cache (derived, lazy scan_visit build, maintenance-erased, Strict-live only) — bit-identical via shared kernel. **@100k×64: host 11.3→1.7 ms; docker vs pgvector 2.36 vs 0.67 ms = 3.5x gap** (was 226x at dawn). Our recall 1.0 vs pg 0.105 at equal probes on this dataset — equal-recall claim needs dataset v2.
- [ ] remaining to parity/beyond: f32 prune blocks + exact double re-rank window (≈2x bytes) · residual profile (~1.7ms: kernel+cands+partial_sort+fetch) · dataset v2 for the honest recall-vs-latency curve · pgvector build 0.4s vs our 4.3s (k-means backfill commits row-by-row — batchable).

## K1 perf rung 10 (2026-07-11, fac46fc) — op-specialised probe kernels

- [x] vec_sq4/vec_dot_nu4/vec_dot4 + per-query vec_nv4 (bit-equal restricted sums — lanes independent; exact path keeps full kernel). 1.7→1.62 ms — loop is LOAD-LATENCY bound now.
- [ ] **NEXT SESSION: f32 prune blocks + exact double re-rank window** (halves bytes, doubles lanes). Window correctness: L2 sums non-negative → relative f32 error ≤ ~n·eps; window = all cands with f32sq ≤ kth_f32sq·(1+2^-18)+eps_abs; dot-ops margin scaled by stored f32 norms × query norm; re-rank window with the exact shared kernel → final top-k bit-exact, gates hold. Store f32 blocks alongside doubles in IvfProbeCache. · also: centroids into the cache (97 samples/query) · batch backfill commits (build 4.3s→) · dataset v2.

## K1 perf rung 11 (2026-07-11) — f32 prune + provable exact re-rank window

- [x] two-phase probe: f32 dense prune (squared-L2 monotone) → want-th pivot → margin window (L2 rel 2^-14 over non-negative sums · cosine abs 1e-4 · IP norm-scaled) → exact double re-rank via shared kernel. Result provably == pure-double path; platform-independent. **@100k×64: host 1.16 ms; docker 1.49 vs pgvector 0.69 = GAP 2.2x** at recall 1.000 vs 0.095.
- [ ] remaining: batch backfill commits (build 4.3s vs 0.4s) · centroids into cache · dataset v2 → publish the recall-vs-latency curve (at equal recall we are likely already ahead — the honest claim needs v2) · residual ~1.1ms profile if pushing to parity on raw latency.

## K1 perf rung 12 (2026-07-12, 838bcc1) — build side

- [x] sampled k-means (deterministic PK-stride, ~50/list — pgvector's rule; stride=1 on small tables → unchanged) + chunked 4096-entry backfill commits. Build @100k×64: 4.3→2.1 s (pgvector 0.4 — rest = full assignment pass + enumeration). Query side-effect: 1.16→0.96 ms (better-balanced lists). Gates hold.
- K1 perf status vs pgvector @100k×64: **query 0.96 vs 0.69 ms raw (1.4x); at recall≥0.95 we are ALONE on the board (his max 0.63-0.71)**; build 2.1 vs 0.4 s (5x).

## K1 perf rung 13 (2026-07-12, cb08188) — winner-fetch seam, hypothesis refuted

- [x] winner point-gets via scan_visit (kept: less work, no Query dependency in ANN tail) — NO wall-clock change (~1.1-1.3ms @probes=10): point-get path was already cheap. Residual = f32 prune stream (~1.3MB/query) + per-candidate bookkeeping — same physics as pgvector; remaining ~1.4x raw = our P-records/window copies/exec pipeline vs bare tuple pointers.
- Raw-parity options if ever needed: flat SoA prune arrays (drop P-structs) · top-want running threshold in prune (skip cold pushes) · probes-downtuning (our recall reserve is huge). The RECALL claim (only system on the board at ≥0.95) stands regardless — bench/compare/vector/REPORT.md.

## K2 BM25 full-text v1 (2026-07-12)

- [x] **K2.1-K2.3 core**: deterministic tokenizer (ASCII, no locale/ICU/stemmer) · USING BM25 posting index ('t'+term+pk → tf,dl denormalised; 'S' → corpus stats, overlay read-modify-write in the row batch; backfill == live path) · top-k `ORDER BY bm25_score(col,'q') DESC LIMIT k` (classic k1=1.2/b=0.75, scan_visit per term, (score DESC, pk) total order) · MATCHES(col,'q') per-row predicate · planner exclusions · K2.6 differential gate = independent reference BM25 in sql_bm25_test (queries × maintenance × columnar × restart).
- [ ] open (K2): projected bm25_score (needs stmt-scoped df/stats memo) · @@ operator sugar · stemmer/language packs (deterministic pinned tables) · K2.4 RRF hybrid helper over CTEs (vectors + BM25 — the flagship demo) · K2.5 relevance parity vs Elasticsearch (BEIR subset) · AT SNAPSHOT reads (leveled posting scans) · phrase/prefix queries.

## K2.4 hybrid RRF (2026-07-12)

- [x] `ORDER BY rrf_score(vec, '[q]', text, 'q') DESC LIMIT k` — fused RRF (k=60) over the two index-backed legs at depth max(60,3k); (score DESC, pk) total order; both-indexes required, clean error otherwise; reference-fusion gate row+columnar.
- [ ] open: leg weights (rrf_score(..., w1, w2)) · projected rrf/bm25 scores · CTE-composable ranks · K2.5 BEIR parity harness.
