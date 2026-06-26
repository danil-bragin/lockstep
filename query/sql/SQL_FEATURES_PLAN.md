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
- [x] C1 COUNT/SUM/AVG(DISTINCT) (per-group dedup in compute_agg; fast paths gated; distributed rejects; teeth) — sql_distinct_agg_test
- [x] B1 LIKE / NOT LIKE (% _; CmpOp::Like + matcher; vectorizer rejects; NOT LIKE wraps Not; teeth) — sql_like_test
- [x] G3 NULLS FIRST/LAST (order_key_less honors per-key; default=NULL smallest unchanged; teeth) — sql_nulls_order_test
- [x] A1 scalar arithmetic (projection) + A2 string fns + A3 CASE + A4 CAST — scalar Expr engine in SELECT projection (row+columnar, == exact, teeth) — sql_expr_test. NOTE: WHERE-side arithmetic = follow-on
- [x] F5 CHECK (column+table level; source-text captured, persisted, re-parsed+eval per write; atomic; durable) — sql_check_test
- [ ] A1/A2/A3/A4 WHERE-side
- [x] D1 UNION/ALL + D2 INTERSECT/EXCEPT (set-op chain; dedup; combined ORDER/LIMIT; arity teeth) — sql_setops_test
- [ ] E2 CROSS
- [ ] D3 FROM-subquery · D4 CTE · F9 types · B2 correlated · C3 window · E3 N-way · E4 non-equi · A4 CAST
- [x] E4 non-equi join — ALREADY PRESENT (theta via nested_loop; verified)
- [ ] F1 composite PK/index (HIGHEST risk — last)
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
