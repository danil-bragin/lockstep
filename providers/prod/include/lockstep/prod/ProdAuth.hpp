#pragma once

// ProdAuth.hpp — APPLICATION-LEVEL IDENTITY + RBAC for the prod client/admin surface.
//
// mTLS (ProdTls) authenticates the CONNECTION (the client cert is verified against the
// shared CA — a wrong-CA / no-cert peer never completes the handshake). This header adds
// the next layer: it maps the authenticated PRINCIPAL (the client certificate subject CN,
// extracted from the verified peer cert in ProdTls) to a ROLE, and a role to a PERMISSION
// set over OPERATION CLASSES. The admin handler (ProdConsensusNode) and the client-facing
// wire::Server consult this BEFORE dispatching any operation; on a permission miss they
// return AUTH-DENIED and NEVER execute the op.
//
// ============================================================================
// MODEL
//   PRINCIPAL  — a string identity = the mTLS client cert subject CN (e.g. "writer-cn").
//                With TLS OFF (plaintext dev mode) the principal is empty ("anonymous").
//   ROLE       — admin | writer | reader (a small fixed ladder).
//   PERMISSION — a bit set over OPERATION CLASSES:
//                  READ  = SELECT / get / scan / STATUS / METRICS  (observe)
//                  WRITE = SUBMIT / INSERT / UPDATE / DELETE        (mutate)
//                  ADMIN = membership / config / shutdown           (control)
//                The ladder is cumulative: reader = {READ}; writer = {READ,WRITE};
//                admin = {READ,WRITE,ADMIN}.
//   POLICY     — an ORDERED principal -> role map, loaded from a config file
//                (`principal=CN role=NAME` lines, or `CN=NAME` shorthand) or set inline.
//                Deterministic: a sorted vector, no ambient state, O(log n) lookup.
//
// ============================================================================
// DEFAULT-DENY / FAIL-CLOSED (the cardinal invariant — the cross-shard lesson):
//   * An UNKNOWN principal (not in the policy) maps to NO role -> every op DENIED.
//   * An UNMAPPED op class (none of READ/WRITE/ADMIN matched) is DENIED, never allowed
//     by fallthrough. allow() returns false unless a role explicitly grants the class.
//   * A malformed / truncated request that does not decode to a known op is DENIED before
//     any dispatch (the caller validates the op class first, never executes on a miss).
//
// PLAINTEXT (TLS off) POLICY — documented dev mode. When the transport is plaintext there
// is no cert, so no principal can be authenticated. The deployment picks a posture via
// AuthPolicy::plaintext_mode:
//   * Enforce (default for a TLS deployment): RBAC is enforced; the anonymous principal
//     gets whatever role (if any) the policy maps "" to — typically NONE -> deny all.
//   * AnonymousReader: plaintext clients are the anonymous principal mapped to `reader`
//     (READ-only) — a convenient dev posture (no writes without a cert).
//   * Open: RBAC disabled entirely (the legacy no-auth path — every op allowed). This is
//     the DEFAULT when no policy is configured at all, so the existing non-auth tests and
//     the plaintext smoke/jepsen runs keep working byte-identically.
//
// PORTABLE: this header is PURE STD C++ — NO OpenSSL, NO sockets, NO __linux__ guard. It
// compiles on the macOS host (so the policy model + its unit logic stay green there). The
// cert-CN EXTRACTION (the only OpenSSL part) lives in ProdTls (TLS-gated); this header just
// consumes the resulting principal STRING. providers/prod is the boundary, but the RBAC
// model itself is provider-agnostic, so the same AuthPolicy is shared by the admin handler
// (prod) and wire::Server (query/, which is NOT lint-exempt and must stay OpenSSL-free).

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lockstep::prod {

// The operation CLASSES a permission set is expressed over. A bit each so a role is a
// cheap bitmask. Unmapped == 0 (no class) -> always denied (fail-closed).
enum class OpClass : std::uint8_t {
    None = 0,
    Read = 1 << 0,   // SELECT / get / scan / STATUS / METRICS — observe
    Write = 1 << 1,  // SUBMIT / INSERT / UPDATE / DELETE — mutate
    Admin = 1 << 2,  // membership / config / shutdown — control
};

[[nodiscard]] inline constexpr std::uint8_t op_bit(OpClass c) noexcept {
    return static_cast<std::uint8_t>(c);
}

// A ROLE = a fixed cumulative permission mask. Returns 0 (no permission) for an unknown
// role name (fail-closed: an unmapped/typo'd role grants nothing).
[[nodiscard]] inline std::uint8_t role_mask(const std::string& role) noexcept {
    const std::uint8_t R = op_bit(OpClass::Read);
    const std::uint8_t W = op_bit(OpClass::Write);
    const std::uint8_t A = op_bit(OpClass::Admin);
    if (role == "reader") {
        return R;
    }
    if (role == "writer") {
        return static_cast<std::uint8_t>(R | W);
    }
    if (role == "admin") {
        return static_cast<std::uint8_t>(R | W | A);
    }
    return 0;  // unknown role -> no permission (DEFAULT-DENY)
}

// How a plaintext (no-cert) connection is treated when a policy IS configured.
enum class PlaintextMode : std::uint8_t {
    Enforce = 0,         // anonymous principal "" mapped per policy (typically deny-all)
    AnonymousReader = 1, // anonymous principal granted READ-only (dev convenience)
    Open = 2,            // RBAC disabled entirely (legacy no-auth path)
};

// ---------------------------------------------------------------------------
// AuthPolicy — the deterministic principal -> role mapping + the enforcement entry point.
// A sorted (principal, role) vector: O(log n) lookup, no ambient state, no nondeterminism.
// `enabled()==false` means NO policy was configured -> RBAC is OPEN (every op allowed) so
// the legacy non-auth deployments are byte-identical. Once any mapping is loaded (or the
// plaintext mode is set non-Open), enforcement is ON.
// ---------------------------------------------------------------------------
class AuthPolicy {
public:
    AuthPolicy() = default;

    // Map a principal (cert CN) to a role name. Keeps the table sorted by principal; a
    // later add for the same principal OVERWRITES (last write wins, deterministic).
    void set(const std::string& principal, const std::string& role) {
        configured_ = true;
        std::size_t pos = 0;
        while (pos < table_.size() && table_[pos].principal < principal) {
            ++pos;
        }
        if (pos < table_.size() && table_[pos].principal == principal) {
            table_[pos].role = role;
            return;
        }
        table_.insert(table_.begin() + static_cast<std::ptrdiff_t>(pos),
                      Entry{principal, role});
    }

    void set_plaintext_mode(PlaintextMode m) noexcept {
        plaintext_mode_ = m;
        if (m != PlaintextMode::Open) {
            configured_ = true;
        }
    }
    [[nodiscard]] PlaintextMode plaintext_mode() const noexcept { return plaintext_mode_; }

    // True iff a policy is configured (any mapping added OR a non-Open plaintext mode set).
    // When false, RBAC is OPEN: allow() returns true for every (principal, op) — the legacy
    // no-auth path, so existing non-auth tests + plaintext smoke/jepsen keep working.
    [[nodiscard]] bool enabled() const noexcept { return configured_; }

    [[nodiscard]] std::size_t size() const noexcept { return table_.size(); }

    // The role name mapped to `principal` (empty string if unmapped). For an EMPTY
    // principal (plaintext / anonymous) under AnonymousReader, this returns "reader".
    [[nodiscard]] std::string role_of(const std::string& principal) const {
        if (principal.empty() && plaintext_mode_ == PlaintextMode::AnonymousReader) {
            return "reader";
        }
        // Binary search the sorted table.
        std::size_t lo = 0;
        std::size_t hi = table_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (table_[mid].principal < principal) {
                lo = mid + 1;
            } else if (principal < table_[mid].principal) {
                hi = mid;
            } else {
                return table_[mid].role;
            }
        }
        return {};  // unmapped principal -> no role (DEFAULT-DENY)
    }

    // THE ENFORCEMENT PREDICATE. Does `principal` have permission for op class `op`?
    //   * RBAC OPEN (no policy) -> ALWAYS true (legacy path).
    //   * Plaintext Open mode -> ALWAYS true (RBAC disabled).
    //   * Otherwise: resolve the role, then test the op-class bit in the role mask. An
    //     unknown principal (role_of=="") yields mask 0 -> false. An OpClass::None op
    //     yields bit 0 -> false. DEFAULT-DENY on every miss (fail-closed).
    [[nodiscard]] bool allow(const std::string& principal, OpClass op) const {
        if (!configured_ || plaintext_mode_ == PlaintextMode::Open) {
            return true;  // RBAC not in force — legacy allow-all.
        }
        if (op == OpClass::None) {
            return false;  // an unmapped/unknown op is DENIED, never allowed by fallthrough.
        }
        const std::uint8_t mask = role_mask(role_of(principal));
        return (mask & op_bit(op)) != 0;
    }

    // Load a policy FILE: one mapping per line. Accepted forms (whitespace-tolerant):
    //   principal=CN role=NAME
    //   CN=NAME                 (shorthand: the principal is the LHS, role the RHS)
    //   plaintext=MODE          (MODE in {enforce, anonymous-reader, anonymous, open})
    // '#' starts a comment; blank lines are ignored. Returns false on a file open error
    // (the caller then REFUSES to run rather than silently run unprotected). A malformed
    // line is skipped (it grants nothing — fail-closed). Deterministic (line order only
    // affects last-write-wins on a duplicate principal).
    bool load_file(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            parse_line(line);
        }
        configured_ = true;
        return true;
    }

    // Parse + apply ONE policy line (also used to apply inline --auth-grant flags). Safe to
    // call with garbage: a line it cannot parse is ignored (no mapping added).
    void parse_line(const std::string& raw) {
        // Strip a trailing comment + surrounding whitespace.
        std::string line = raw;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        std::string principal;
        std::string role;
        std::string plaintext;
        std::istringstream ss(line);
        std::string tok;
        bool shorthand = true;
        std::string sh_lhs;
        std::string sh_rhs;
        while (ss >> tok) {
            const std::size_t eq = tok.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = tok.substr(0, eq);
            const std::string val = tok.substr(eq + 1);
            if (key == "principal") {
                principal = val;
                shorthand = false;
            } else if (key == "role") {
                role = val;
                shorthand = false;
            } else if (key == "plaintext") {
                plaintext = val;
                shorthand = false;
            } else if (shorthand && sh_lhs.empty()) {
                sh_lhs = key;
                sh_rhs = val;
            }
        }
        if (!plaintext.empty()) {
            if (plaintext == "enforce") {
                set_plaintext_mode(PlaintextMode::Enforce);
            } else if (plaintext == "anonymous-reader" || plaintext == "anonymous") {
                set_plaintext_mode(PlaintextMode::AnonymousReader);
            } else if (plaintext == "open") {
                set_plaintext_mode(PlaintextMode::Open);
            }
            return;
        }
        if (shorthand) {
            if (!sh_lhs.empty() && !sh_rhs.empty()) {
                set(sh_lhs, sh_rhs);
            }
            return;
        }
        if (!principal.empty() && !role.empty()) {
            set(principal, role);
        }
    }

private:
    struct Entry {
        std::string principal;  // the cert CN
        std::string role;       // admin | writer | reader
    };
    std::vector<Entry> table_{};   // sorted by principal (deterministic, O(log n) lookup)
    bool configured_ = false;      // any mapping/mode loaded? false == RBAC OPEN (legacy)
    PlaintextMode plaintext_mode_ = PlaintextMode::Enforce;
};

}  // namespace lockstep::prod
