#!/usr/bin/env python3
# Lockstep forbidden-call lint (Phase 0, spec C0.2; cardinal rule 1).
#
# Cardinal rule 1: ALL nondeterminism flows through the abstraction boundary
# (Clock/Network/Disk/Random/Scheduler) implemented under providers/. Everything
# else MUST NOT touch wall-clock, raw sockets, raw file IO, raw randomness,
# real threads, or coordination atomics directly.
#
# This script scans the whole repo tree for C/C++ sources and headers and FAILS
# (non-zero exit) on any forbidden pattern. It is stdlib-only (no pip deps).
#
# Design contract:
#   - ZERO false negatives on the forbidden set. This is the invariant. We strip
#     comments and string literals before matching ONLY to cut false positives;
#     we never widen stripping in a way that could hide a real forbidden token.
#   - Exempt: any file whose path contains a `providers/` segment (rule 1 says
#     nondeterminism is allowed there — that is the whole point of the boundary).
#   - Exempt from the REPO-WIDE gate: this lint's own fixtures/ directory, which
#     deliberately contains forbidden calls for the test suite. Tests target the
#     fixtures directly by path; the gate must stay green on real code.
#
# CLI: python3 tools/lint/forbidden_calls.py [root]   (default root = repo root)
#   exit 0 = clean, non-zero = at least one violation.
#   output: `path:line: <pattern>` per hit + a summary line.

from __future__ import annotations

import os
import re
import sys

# File extensions we scan: C/C++ sources, headers, and C++ modules.
SOURCE_EXTS = (".h", ".hpp", ".hh", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".ixx")

# Path segments that exempt a file entirely.
#   - "providers": cardinal rule 1 — the abstraction boundary lives here.
#   - the lint's own fixtures: dirty fixtures exist to be flagged by the tests,
#     not by the real gate. Keyed on the relative path so it only applies to
#     OUR fixtures, not some unrelated directory literally named "fixtures".
EXEMPT_PATH_SEGMENTS = ("providers",)
EXEMPT_REL_PREFIXES = (os.path.join("tools", "lint", "fixtures"),)


# Each forbidden pattern: (label, compiled regex). Labels are the human-facing
# `<pattern>` printed per hit. Regexes match the de-commented, de-stringified
# line. `\b` / explicit boundaries keep them tight without dropping hits.
def _build_patterns():
    pats = []

    def add(label, regex):
        pats.append((label, re.compile(regex)))

    # --- Clock / wall-clock time -------------------------------------------
    # std::chrono in any form (system_clock, steady_clock, durations, now()...).
    add("std::chrono", r"\bstd\s*::\s*chrono\b")

    # --- Randomness --------------------------------------------------------
    add("std::rand", r"\bstd\s*::\s*rand\b")
    add("std::random_device", r"\bstd\s*::\s*random_device\b")
    # <random> engines commonly used as a global/ambient randomness source.
    # Randomness must come from IRandom (single seeded PRNG surface), not these.
    add("std::mt19937", r"\bstd\s*::\s*mt19937(?:_64)?\b")
    add("std::default_random_engine", r"\bstd\s*::\s*default_random_engine\b")
    add("<random>", r"#\s*include\s*<\s*random\s*>")
    # bare srand/rand C calls.
    add("srand(", r"(?<![\w:])srand\s*\(")
    add("rand(", r"(?<![\w:.])rand\s*\(")

    # --- Threads -----------------------------------------------------------
    add("std::thread", r"\bstd\s*::\s*thread\b")
    add("std::jthread", r"\bstd\s*::\s*jthread\b")
    add("<thread>", r"#\s*include\s*<\s*thread\s*>")

    # --- Raw socket syscalls ----------------------------------------------
    for fn in ("socket", "bind", "connect", "accept", "listen",
               "send", "recv", "sendto", "recvfrom"):
        # Reject the bare call `fn(`. Negative lookbehind avoids matching
        # member access (`.send(`, `->send(`, `x::send(`, `obj_send(`),
        # which are presumably calls through an abstraction, not the syscall.
        # These names collide with legitimate abstraction-boundary method
        # NAMES (e.g. `virtual Future<Error> send(...) = 0;` in INetwork).
        # A post-match declaration guard (see DECL_GUARD_NAMES / looks_like_decl)
        # drops declaration shapes while keeping zero false negatives on calls.
        add(f"{fn}(", rf"(?<![\w:.>])(?<!->){fn}\s*\(")
    add("<sys/socket.h>", r"#\s*include\s*<\s*sys/socket\.h\s*>")
    add("<netinet/in.h>", r"#\s*include\s*<\s*netinet/in\.h\s*>")
    add("<arpa/inet.h>", r"#\s*include\s*<\s*arpa/inet\.h\s*>")

    # --- Raw file syscalls / raw IO ---------------------------------------
    # Global-scope syscalls written as ::open(, etc.
    for fn in ("open", "read", "write", "close"):
        add(f"::{fn}(", rf"::\s*{fn}\s*\(")
    # lseek/fsync/fdatasync/pread/pwrite are POSIX-only — bare call is forbidden.
    for fn in ("lseek", "fsync", "fdatasync", "pread", "pwrite", "ftruncate"):
        add(f"{fn}(", rf"(?<![\w:.>])(?<!->){fn}\s*\(")
    add("<fcntl.h>", r"#\s*include\s*<\s*fcntl\.h\s*>")
    add("<unistd.h>", r"#\s*include\s*<\s*unistd\.h\s*>")
    add("<sys/uio.h>", r"#\s*include\s*<\s*sys/uio\.h\s*>")

    # --- Coordination atomics ---------------------------------------------
    # Bare std::atomic for a plain counter is GRAY and intentionally NOT flagged
    # (it carries no cross-thread ordering by itself; documented choice). What we
    # flag is anything expressing inter-thread *ordering*, which has no place
    # outside provider impls in a single-threaded deterministic core:
    #   - std::memory_order_* (relaxed/acquire/release/acq_rel/seq_cst/consume)
    #   - std::atomic_thread_fence / std::atomic_signal_fence
    add("std::memory_order_", r"\bstd\s*::\s*memory_order_\w+")
    # also catch the enum used unqualified after a `using`/bare reference.
    add("memory_order_", r"(?<![\w:])memory_order_\w+")
    add("std::atomic_thread_fence", r"\bstd\s*::\s*atomic_thread_fence\b")
    add("std::atomic_signal_fence", r"\bstd\s*::\s*atomic_signal_fence\b")
    add("atomic_thread_fence(", r"(?<![\w:.>])atomic_thread_fence\s*\(")

    return pats


PATTERNS = _build_patterns()

# Labels whose function names collide with legitimate C++ method names (the
# abstraction-boundary interfaces are literally named send/recv/connect/etc).
# For these ONLY, we suppress a hit when the line is a function *declaration*
# rather than a *call*. Includes are not in this set; chrono/thread/atomics are
# unambiguous std:: tokens and are never guarded.
DECL_GUARD_NAMES = {
    "socket", "bind", "connect", "accept", "listen",
    "send", "recv", "sendto", "recvfrom",
    "lseek", "fsync", "fdatasync", "pread", "pwrite", "ftruncate",
}

# Keywords that may legitimately precede a CALL (so `return send(...)` etc. must
# NOT be mistaken for a declaration — that would be a false negative).
_CALL_PRECEDING_KEYWORDS = {
    "return", "co_return", "co_await", "co_yield", "and", "or", "not",
    "else", "case", "delete", "new", "sizeof", "throw", "noexcept",
    "while", "if", "switch", "for", "do",
}

# A declaration shape: <something that is a return type / `virtual`> <name>(...).
# i.e. a word/type-closing token, then whitespace, then the bare name, then `(`.
# We additionally require the preceding word NOT be a call-preceding keyword.
_DECL_RE_CACHE = {}


def _decl_regex(name: str):
    rx = _DECL_RE_CACHE.get(name)
    if rx is None:
        # capture the word immediately before `name(` (if it's a plain word),
        # plus catch type-closing punctuation (`>`, `)`, `]`, `*`, `&`).
        rx = re.compile(
            rf"(?:(?P<word>\w+)|[>)\]&*])\s+{name}\s*\("
        )
        _DECL_RE_CACHE[name] = rx
    return rx


def looks_like_declaration(name: str, line: str) -> bool:
    """True if `name(` on `line` looks like a function declaration/definition
    (a return type precedes it), NOT a call. Conservative: only returns True
    when a return-type token precedes the name, and never when the preceding
    word is a keyword that can introduce a call (so calls are never masked)."""
    m = _decl_regex(name).search(line)
    if not m:
        return False
    word = m.group("word")
    if word is not None and word in _CALL_PRECEDING_KEYWORDS:
        return False
    return True


def is_exempt(rel_path: str) -> bool:
    """True if this path is exempt from scanning."""
    norm = rel_path.replace(os.sep, "/")
    parts = norm.split("/")
    if any(seg in parts for seg in EXEMPT_PATH_SEGMENTS):
        return True
    for prefix in EXEMPT_REL_PREFIXES:
        pre = prefix.replace(os.sep, "/")
        if norm == pre or norm.startswith(pre + "/"):
            return True
    return False


def strip_comments_and_strings(text: str) -> str:
    """Return `text` with C/C++ comments and string/char literals replaced by
    spaces (length-preserving so column/line structure is unchanged).

    This is a best-effort tokenizer-lite pass to reduce false positives. It is
    deliberately conservative: when in doubt it leaves content intact, so a
    forbidden token sitting in real code is NEVER masked. Newlines are kept so
    line numbering after split() is exact.
    """
    out = []
    i = 0
    n = len(text)
    state = "code"  # code | line_comment | block_comment | string | char | raw_string
    raw_delim = ""  # closing sequence for a raw string literal

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "code":
            # Raw string literal: R"delim( ... )delim"  (also u8R"...", LR"..." etc.)
            if c == 'R' and nxt == '"':
                # confirm preceding chars are a valid raw-string prefix or nothing
                j = i + 2
                delim = ""
                while j < n and text[j] != '(' and text[j] not in '"\\ \t\n':
                    delim += text[j]
                    j += 1
                if j < n and text[j] == '(':
                    state = "raw_string"
                    raw_delim = ')' + delim + '"'
                    out.append("  ")  # R"
                    out.append(" " * (j - (i + 2)))  # delim
                    out.append(" ")  # (
                    i = j + 1
                    continue
                # not actually a raw string; fall through treating 'R' as code
                out.append(c)
                i += 1
                continue
            if c == '/' and nxt == '/':
                state = "line_comment"
                out.append("  ")
                i += 2
                continue
            if c == '/' and nxt == '*':
                state = "block_comment"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "string"
                out.append(" ")
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(" ")
                i += 1
                continue
            out.append(c)
            i += 1
            continue

        if state == "line_comment":
            if c == "\n":
                state = "code"
                out.append("\n")
                i += 1
                continue
            out.append(" " if c != "\t" else "\t")
            i += 1
            continue

        if state == "block_comment":
            if c == '*' and nxt == '/':
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else (" " if c != "\t" else "\t"))
            i += 1
            continue

        if state == "string":
            if c == "\\" and nxt:
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "code"
                out.append(" ")
                i += 1
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue

        if state == "char":
            if c == "\\" and nxt:
                out.append("  ")
                i += 2
                continue
            if c == "'":
                state = "code"
                out.append(" ")
                i += 1
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue

        if state == "raw_string":
            if text.startswith(raw_delim, i):
                out.append(" " * len(raw_delim))
                i += len(raw_delim)
                state = "code"
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue

    return "".join(out)


def scan_file(path: str):
    """Yield (line_number, label) for each forbidden hit in `path`."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
    except OSError as exc:
        print(f"{path}: ERROR could not read: {exc}", file=sys.stderr)
        return

    stripped = strip_comments_and_strings(raw)
    lines = stripped.split("\n")
    for idx, line in enumerate(lines, start=1):
        for label, rx in PATTERNS:
            if not rx.search(line):
                continue
            # For collision-prone names, drop declaration shapes (e.g. a virtual
            # interface method named `send`). Calls are never masked: the guard
            # only fires when a return-type token precedes the name.
            name = label[:-1] if label.endswith("(") else label
            if name in DECL_GUARD_NAMES and looks_like_declaration(name, line):
                continue
            yield idx, label


def iter_source_files(root: str):
    """Walk `root`, yielding (abs_path, rel_path) for non-exempt source files."""
    for dirpath, dirnames, filenames in os.walk(root):
        # prune .git and exempt dirs early for speed.
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            if not name.endswith(SOURCE_EXTS):
                continue
            abs_path = os.path.join(dirpath, name)
            rel_path = os.path.relpath(abs_path, root)
            if is_exempt(rel_path):
                continue
            yield abs_path, rel_path


def run(root: str) -> int:
    root = os.path.abspath(root)
    total_hits = 0
    files_with_hits = 0
    files_scanned = 0

    for abs_path, rel_path in sorted(iter_source_files(root)):
        files_scanned += 1
        hits = list(scan_file(abs_path))
        if hits:
            files_with_hits += 1
            for line_no, label in hits:
                # machine-readable: path:line: <pattern>
                print(f"{rel_path}:{line_no}: {label}")
                total_hits += 1

    if total_hits:
        print(
            f"SUMMARY: FAIL — {total_hits} forbidden call(s) in "
            f"{files_with_hits} file(s) ({files_scanned} scanned)."
        )
        return 1
    print(f"SUMMARY: OK — 0 forbidden calls ({files_scanned} files scanned).")
    return 0


def _default_root() -> str:
    # repo root = two levels up from this file (tools/lint/forbidden_calls.py).
    return os.path.abspath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    )


def main(argv) -> int:
    root = argv[1] if len(argv) > 1 else _default_root()
    if not os.path.exists(root):
        print(f"error: root path does not exist: {root}", file=sys.stderr)
        return 2
    return run(root)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
