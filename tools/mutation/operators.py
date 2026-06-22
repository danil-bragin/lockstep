#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Lockstep mutation operators — the documented operator set.

WHY THIS EXISTS
---------------
Per master-plan §6.7, mutation testing is the *adequacy meta-gate*: because every
line of test code in Lockstep is agent-written, the only mechanical proof the
tests are not "coverage theater" is that they KILL deliberately injected bugs.
This module produces those bugs (mutants) deterministically from C++ source.

DESIGN
------
* stdlib only — no third-party deps, no real C++ parser. We work on the token
  stream of each source LINE, skipping comments, string/char literals, and
  preprocessor directives so we never mutate inside a quoted string or a
  `#include`. This is a *lexical* mutation engine: cheaper than a full AST,
  and sufficient to plant semantically-meaningful single-edit bugs.
* Each mutant is exactly ONE source edit (one operator applied at one site).
* Generation is a pure function of (file bytes, operator set). Ordering is
  fully deterministic: files sorted by path, sites scanned line-then-column,
  operators applied in a fixed registry order. Selection/sampling is seeded
  separately (see run_mutation.py) — never system randomness.

OPERATOR SET (documented; mirrored in docs/mutation.md)
-------------------------------------------------------
  ROR  relational-operator replacement : <  <= >  >=   (each → the others, swap)
  EQ   equality-operator replacement   : == ↔ !=
  LCR  logical-connector replacement   : && ↔ ||
  AOR  arithmetic-operator replacement : + ↔ -, * ↔ /
  ABS  integer-literal boundary/off-by-one : N → N+1 and N → N-1
  CBR  constant/boolean replacement    : true ↔ false
  NEG  negate-condition                : `if (X)` → `if (!(X))`
  SDL  statement deletion (safe)       : a one-line `trace(...);` style call stmt
                                         → replaced with `;` (no-op)

Every operator records: file, 1-based line, 1-based column, operator code, the
exact original text, and the mutated text. A mutant's identity is stable across
runs, which is what makes the whole engine replayable.
"""

from __future__ import annotations

import re
from dataclasses import dataclass


@dataclass(frozen=True)
class Mutant:
    """One injected bug: a single-edit description, fully replayable."""

    file_path: str        # repo-relative path of the mutated file
    line: int             # 1-based line number of the edit
    col: int              # 1-based column (within the line) of the edit
    op: str               # operator code (ROR / EQ / LCR / AOR / ABS / CBR / NEG / SDL)
    original: str         # exact original token/snippet
    mutated: str          # the replacement token/snippet
    new_line_text: str    # the full line AFTER the edit (what gets written)

    @property
    def mutant_id(self) -> str:
        # Stable, human-readable, sortable id. file:line:col:op:mutated keeps it
        # unique even when one site hosts several mutants of the same operator
        # (e.g. ABS produces both N+1 and N-1 at the same column).
        safe = self.mutated.replace(":", "_").replace(" ", "")
        return f"{self.file_path}:{self.line}:{self.col}:{self.op}:{safe}"

    def describe(self) -> str:
        return f"{self.op}  '{self.original}' -> '{self.mutated}'"


# ---------------------------------------------------------------------------
# Lexical masking — find the regions of a line we MUST NOT mutate inside:
# comments, string/char literals. Returns a boolean mask (True = code, safe).
# ---------------------------------------------------------------------------
def _code_mask(line: str, in_block_comment: bool) -> tuple[list[bool], bool]:
    """Return (mask, still_in_block_comment).

    mask[i] is True iff column i of `line` is ordinary code (not inside a //
    comment, /* */ comment, "string", or 'char' literal). Handles escapes.
    """
    n = len(line)
    mask = [True] * n
    i = 0
    state = "block" if in_block_comment else "code"
    while i < n:
        c = line[i]
        nxt = line[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                # line comment to end of line
                for j in range(i, n):
                    mask[j] = False
                return mask, False
            if c == "/" and nxt == "*":
                mask[i] = False
                mask[i + 1] = False
                i += 2
                state = "block"
                continue
            if c == '"':
                mask[i] = False
                state = "string"
                i += 1
                continue
            if c == "'":
                mask[i] = False
                state = "char"
                i += 1
                continue
            i += 1
            continue
        if state == "block":
            mask[i] = False
            if c == "*" and nxt == "/":
                mask[i + 1] = False
                i += 2
                state = "code"
                continue
            i += 1
            continue
        if state == "string":
            mask[i] = False
            if c == "\\":
                if i + 1 < n:
                    mask[i + 1] = False
                i += 2
                continue
            if c == '"':
                state = "code"
            i += 1
            continue
        if state == "char":
            mask[i] = False
            if c == "\\":
                if i + 1 < n:
                    mask[i + 1] = False
                i += 2
                continue
            if c == "'":
                state = "code"
            i += 1
            continue
    return mask, (state == "block")


def _is_code_span(mask: list[bool], start: int, end: int) -> bool:
    return all(mask[k] for k in range(start, end))


# ---------------------------------------------------------------------------
# Token operators. Each returns a list of (col, original, mutated) for the line.
# We match on word/operator boundaries so we don't mutate inside identifiers.
# ---------------------------------------------------------------------------

# Relational / equality / logical / arithmetic operator swaps. The ORDER of keys
# in these dicts is part of the deterministic contract (Python preserves
# insertion order). Longer operators are checked before their prefixes so we
# never mis-tokenize `<=` as `<`.
_ROR_SWAPS = {
    "<=": ">",   # boundary-flipping relational swaps
    ">=": "<",
    "<": ">=",
    ">": "<=",
}
_EQ_SWAPS = {
    "==": "!=",
    "!=": "==",
}
_LCR_SWAPS = {
    "&&": "||",
    "||": "&&",
}
_AOR_SWAPS = {
    "+": "-",
    "-": "+",
    "*": "/",
    "/": "*",
}


def _find_operator_sites(line: str, mask: list[bool], swaps: dict[str, str], op_code: str):
    """Yield (col, original, mutated, new_line) for each operator occurrence.

    Operators are matched greedily longest-first at each position so `<=` is one
    token, not `<` then `=`. We skip occurrences that are part of a longer C++
    token (e.g. `<<`, `->`, `+=`, `/* */`, `//`) to avoid invalid mutations.
    """
    out = []
    keys = sorted(swaps.keys(), key=len, reverse=True)
    i = 0
    n = len(line)
    while i < n:
        if not mask[i]:
            i += 1
            continue
        matched = False
        for k in keys:
            kl = len(k)
            if line[i:i + kl] != k:
                continue
            if not _is_code_span(mask, i, i + kl):
                continue
            prev = line[i - 1] if i > 0 else ""
            after = line[i + kl] if i + kl < n else ""
            # Reject if this is part of a longer operator token.
            # e.g. `<<`, `>>`, `<=`/`>=` (already separate), `->`, compound
            # assignment `+=`/`-=`/`*=`/`/=`, `++`/`--`, `==`/`!=`, `&&`/`||`,
            # `//` and `/*` comments, `::`, etc.
            neighbour_op = set("+-*/<>=&|")
            if op_code == "AOR":
                # arithmetic: must not be ++, --, +=, -=, *=, /=, //, /*, */, ->,
                # *p deref-like contexts are still valid binary in many spots but
                # to stay safe we only mutate when both sides look like operands.
                if k in ("+", "-"):
                    if after in ("+", "-", "=") or prev in ("+", "-"):
                        continue
                    # unary +/-: preceded by an operator or open paren/comma/return
                    pj = i - 1
                    while pj >= 0 and line[pj] == " ":
                        pj -= 1
                    pchar = line[pj] if pj >= 0 else ""
                    if pchar in set("(,=<>!&|*/%+-?:;{") or pchar == "":
                        continue
                    # also skip `return -x` / `= -x` handled above via pchar
                if k in ("*", "/"):
                    if after in ("=", "*", "/") or prev in ("*", "/"):
                        continue
                    # pointer/deref/multiply ambiguity: require an operand char to
                    # the left (digit, identifier char, or close paren/bracket).
                    pj = i - 1
                    while pj >= 0 and line[pj] == " ":
                        pj -= 1
                    pchar = line[pj] if pj >= 0 else ""
                    if not (pchar.isalnum() or pchar in "_)]"):
                        continue
            else:
                if prev in neighbour_op and prev not in ("",):
                    # e.g. `<<`, `&&` boundaries — but for LCR/EQ the whole token
                    # is already `&&`/`==`; guard only single-char relational.
                    if op_code == "ROR" and k in ("<", ">"):
                        continue
                if op_code == "ROR" and k in ("<", ">"):
                    # skip shifts and template-ish `<<`/`>>` and `<=`/`>=`
                    if after in ("<", ">", "=") or prev in ("<", ">"):
                        continue
                    # skip `->` (prev `-`) and template angle brackets heuristically:
                    # a `<` immediately preceded by an identifier char AND followed
                    # by an identifier char is likely a template — but in this
                    # codebase comparisons are spaced. Require a space on at least
                    # one side to be a comparison.
                    if i > 0 and i + kl < n and line[i - 1] != " " and line[i + kl] != " ":
                        continue
            mutated = swaps[k]
            new_line = line[:i] + mutated + line[i + kl:]
            out.append((i + 1, k, mutated, new_line))
            i += kl
            matched = True
            break
        if not matched:
            i += 1
    return out


# Integer literal boundary / off-by-one. We match decimal integer literals that
# are standalone operands (not part of an identifier, not hex, not float).
_INT_RE = re.compile(r"(?<![\w.])(\d+)(?:[uUlL]*)(?![\w.])")


def _find_literal_sites(line: str, mask: list[bool]):
    out = []
    for m in _INT_RE.finditer(line):
        start = m.start(1)
        end = m.end(1)
        if not _is_code_span(mask, start, end):
            continue
        text = m.group(1)
        # Skip the common 0/1 that are usually structural (array index base,
        # bit shifts); still mutate them — off-by-one on 0 and 1 is exactly the
        # kind of bug we want killed. But skip absurdly long literals (hashes,
        # magic constants) to keep mutants meaningful and compiling.
        if len(text) > 6:
            continue
        try:
            val = int(text)
        except ValueError:
            continue
        for delta, tag in ((1, "+1"), (-1, "-1")):
            new_val = val + delta
            if new_val < 0:
                continue
            mutated = str(new_val)
            new_line = line[:start] + mutated + line[end:]
            out.append((start + 1, text, mutated, new_line, tag))
    return out


# Boolean constant replacement: true <-> false as standalone keywords.
_BOOL_RE = re.compile(r"(?<![\w])(true|false)(?![\w])")


def _find_bool_sites(line: str, mask: list[bool]):
    out = []
    swap = {"true": "false", "false": "true"}
    for m in _BOOL_RE.finditer(line):
        start, end = m.start(1), m.end(1)
        if not _is_code_span(mask, start, end):
            continue
        text = m.group(1)
        mutated = swap[text]
        new_line = line[:start] + mutated + line[end:]
        out.append((start + 1, text, mutated, new_line))
    return out


# Negate-condition: `if (` ... `)` on a single line -> wrap with !(...). We only
# handle simple single-line `if (cond) {` / `if (cond)` to stay compiling.
_IF_RE = re.compile(r"\bif\s*\(")
_WHILE_RE = re.compile(r"\bwhile\s*\(")


def _match_paren(line: str, open_idx: int) -> int:
    """Return index of the matching ')' for the '(' at open_idx, or -1."""
    depth = 0
    for j in range(open_idx, len(line)):
        if line[j] == "(":
            depth += 1
        elif line[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def _find_negate_sites(line: str, mask: list[bool]):
    out = []
    for rx in (_IF_RE, _WHILE_RE):
        for m in rx.finditer(line):
            open_idx = line.index("(", m.start())
            if not mask[open_idx]:
                continue
            close_idx = _match_paren(line, open_idx)
            if close_idx < 0 or not mask[close_idx]:
                continue
            cond = line[open_idx + 1:close_idx]
            if not cond.strip():
                continue
            # Avoid double-negating an already-negated wrapped condition.
            mutated_cond = "!(" + cond + ")"
            new_line = line[:open_idx + 1] + mutated_cond + line[close_idx:]
            out.append((open_idx + 2, cond.strip(), "!(" + cond.strip() + ")", new_line))
    return out


# Statement deletion (SAFE subset): a line that is exactly a single call
# statement we can drop without breaking control flow — specifically `trace(...);`
# and `*.push_back(...);` style side-effect calls, or a standalone `++x;`.
# Replacing with an empty statement keeps the brace/line structure intact.
_SAFE_DELETE_RE = re.compile(
    r"^(\s*)(?:[\w:.>()-]+\s*->\s*)?[\w:.]+\s*(?:\.|->)?[\w]*\([^;{}]*\)\s*;\s*$"
)


def _find_delete_sites(line: str, mask: list[bool]):
    out = []
    m = _SAFE_DELETE_RE.match(line)
    if not m:
        return out
    indent = m.group(1)
    stripped = line.strip()
    # Don't delete `return ...;` (changes type/flow) or assignments (=) or
    # declarations. Only pure expression-statement calls with no '='.
    if "=" in stripped or stripped.startswith("return") or stripped.startswith("#"):
        return out
    # Must actually be code (the whole line).
    if not any(mask):
        return out
    new_line = indent + ";"
    out.append((1, stripped, ";", new_line))
    return out


# ---------------------------------------------------------------------------
# Per-file mutant generation. Deterministic order: line ascending, then by the
# operator registry order below, then by column ascending.
# ---------------------------------------------------------------------------
_OPERATOR_REGISTRY = ("ROR", "EQ", "LCR", "AOR", "ABS", "CBR", "NEG", "SDL")


def generate_for_file(repo_rel_path: str, source_text: str) -> list[Mutant]:
    """Produce the full deterministic mutant list for one source file."""
    lines = source_text.split("\n")
    in_block = False
    mutants: list[Mutant] = []
    for lineno, raw in enumerate(lines, start=1):
        mask, in_block = _code_mask(raw, in_block)
        stripped = raw.lstrip()
        # Never mutate preprocessor lines (#include / #define / #if ...).
        if stripped.startswith("#"):
            continue

        line_bucket: list[Mutant] = []

        for col, orig, mut, new_line in _find_operator_sites(raw, mask, _ROR_SWAPS, "ROR"):
            line_bucket.append(Mutant(repo_rel_path, lineno, col, "ROR", orig, mut, new_line))
        for col, orig, mut, new_line in _find_operator_sites(raw, mask, _EQ_SWAPS, "EQ"):
            line_bucket.append(Mutant(repo_rel_path, lineno, col, "EQ", orig, mut, new_line))
        for col, orig, mut, new_line in _find_operator_sites(raw, mask, _LCR_SWAPS, "LCR"):
            line_bucket.append(Mutant(repo_rel_path, lineno, col, "LCR", orig, mut, new_line))
        for col, orig, mut, new_line in _find_operator_sites(raw, mask, _AOR_SWAPS, "AOR"):
            line_bucket.append(Mutant(repo_rel_path, lineno, col, "AOR", orig, mut, new_line))
        for col, orig, mut, new_line, _tag in _find_literal_sites(raw, mask):
            line_bucket.append(
                Mutant(repo_rel_path, lineno, col, "ABS", orig, mut, new_line)
            )
        for col, orig, mut, new_line in _find_bool_sites(raw, mask):
            line_bucket.append(Mutant(repo_rel_path, lineno, col, "CBR", orig, mut, new_line))
        for col, orig, mut, new_line in _find_negate_sites(raw, mask):
            line_bucket.append(Mutant(repo_rel_path, lineno, col, "NEG", orig, mut, new_line))
        for col, orig, mut, new_line in _find_delete_sites(raw, mask):
            line_bucket.append(Mutant(repo_rel_path, lineno, col, "SDL", orig, mut, new_line))

        # Stable sort within the line: by operator registry rank, then column.
        rank = {op: r for r, op in enumerate(_OPERATOR_REGISTRY)}
        line_bucket.sort(key=lambda mu: (rank[mu.op], mu.col, mu.mutated))
        mutants.extend(line_bucket)

    return mutants


OPERATOR_DOC = {
    "ROR": "relational operator replacement: < <= > >= swapped",
    "EQ": "equality operator replacement: == <-> !=",
    "LCR": "logical connector replacement: && <-> ||",
    "AOR": "arithmetic operator replacement: + <-> -, * <-> /",
    "ABS": "integer-literal boundary / off-by-one: N -> N+1, N -> N-1",
    "CBR": "constant/boolean replacement: true <-> false",
    "NEG": "negate condition: if(X)/while(X) -> if(!(X))",
    "SDL": "statement deletion: a safe side-effect call statement -> ;",
}
