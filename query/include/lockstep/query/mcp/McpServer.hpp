#pragma once

// McpServer.hpp — K11.1: the agent-memory MCP surface. One session speaks MCP's
// stdio framing (newline-delimited JSON-RPC 2.0) and serves five tools over a
// SqlEngine — composing K1 vectors + K2 BM25/RRF + K4 Seq cursors + K10 AS OF
// into the complete agent-memory stack in ONE engine:
//
//   query    {sql}                          — run SQL, rows back as JSON
//   schema   {}                             — table + column introspection
//   remember {content, embedding?, kind?}   — durable memory write (auto-provisions
//                                             the memory table + IVFFLAT + BM25)
//   recall   {query, embedding?, k?}        — hybrid RRF (vectors+BM25) or BM25-only
//   history  {sql, seq}                     — the SELECT as of Seq n ("what did the
//                                             agent know at step N?" — exactly)
//
// The two properties nobody else has fall out of the engine: recall is
// DETERMINISTIC (same memory + same query = byte-identical ranking, replayable
// in an incident review) and history is EXACT (Seq is the engine's own version
// line, not a wall-clock approximation).
//
// Transport notes: JSON parsing/serialization reuses the SQL layer's canonical
// JVal (sql/Json.hpp). A request with an `id` gets exactly one response line; a
// notification gets none. Parse errors -> -32700, unknown method -> -32601 (the
// JSON-RPC contract); TOOL failures are NOT protocol errors — they return
// result.isError=true with the message as text content (the MCP convention).

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/sql/Json.hpp>

namespace lockstep::query::mcp {

using sql::json::JVal;

class McpSession {
public:
    // K11.4: per-agent isolation. A non-empty agent name pins the session to its OWN
    // schema (CREATE SCHEMA agent_<name> + search_path) — every tool then reads and
    // writes that agent's memory only; agents cannot see each other's stores. The
    // name is [A-Za-z0-9_]+ (validated); "" = the shared default schema.
    explicit McpSession(sql::SqlEngine& engine, std::string agent = {})
        : eng_(&engine), agent_(std::move(agent)) {
        if (!agent_.empty()) {
            bool ok = true;
            for (const char c : agent_) {
                if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') ok = false;
            }
            if (ok) {
                (void)eng_->exec("CREATE SCHEMA IF NOT EXISTS agent_" + agent_);
                (void)eng_->exec("SET search_path TO agent_" + agent_);
            } else {
                agent_.clear();  // invalid name: fall back to the shared schema
            }
        }
    }

    // One inbound line -> one outbound line ("" for notifications / no response).
    [[nodiscard]] std::string handle_line(const std::string& line) {
        JVal req;
        std::size_t p = 0;
        if (!sql::json::parse_value(line, p, req) || req.kind != JVal::Obj) {
            return error_line(JVal{}, -32700, "parse error");
        }
        const JVal* id = find(req, "id");
        const JVal* method = find(req, "method");
        if (method == nullptr || method->kind != JVal::Str) {
            return error_line(id ? *id : JVal{}, -32600, "invalid request");
        }
        const std::string& m = method->text;
        const JVal* params = find(req, "params");
        if (m == "initialize") return initialize_line(*id);
        if (m == "notifications/initialized" || m.rfind("notifications/", 0) == 0) return "";
        if (m == "ping") return result_line(*id, obj({}));
        if (m == "tools/list") return tools_list_line(*id);
        if (m == "tools/call") return tools_call_line(*id, params);
        return error_line(id ? *id : JVal{}, -32601, "method not found: " + m);
    }

private:
    // ---- JVal helpers -----------------------------------------------------
    [[nodiscard]] static const JVal* find(const JVal& o, const std::string& key) {
        for (const JVal::Member& mem : o.obj) {
            if (mem.key == key) return &mem.val;
        }
        return nullptr;
    }
    [[nodiscard]] static JVal str(const std::string& s) {
        JVal v;
        v.kind = JVal::Str;
        v.text = s;
        return v;
    }
    [[nodiscard]] static JVal num(std::int64_t n) {
        JVal v;
        v.kind = JVal::Num;
        v.text = std::to_string(n);
        return v;
    }
    [[nodiscard]] static JVal boolean(bool b) {
        JVal v;
        v.kind = JVal::Bool;
        v.b = b;
        return v;
    }
    [[nodiscard]] static JVal obj(std::vector<JVal::Member> members) {
        JVal v;
        v.kind = JVal::Obj;
        v.obj = std::move(members);
        return v;
    }
    [[nodiscard]] static JVal arr(std::vector<JVal> items) {
        JVal v;
        v.kind = JVal::Arr;
        v.arr = std::move(items);
        return v;
    }
    [[nodiscard]] static std::string render(const JVal& v) {
        std::string out;
        sql::json::serialize(v, out);
        return out;
    }

    [[nodiscard]] static std::string result_line(const JVal& id, JVal result) {
        return render(obj({{"jsonrpc", str("2.0")},
                           {"id", id},
                           {"result", std::move(result)}}));
    }
    [[nodiscard]] static std::string error_line(const JVal& id, std::int64_t code,
                                                const std::string& msg) {
        return render(obj({{"jsonrpc", str("2.0")},
                           {"id", id},
                           {"error", obj({{"code", num(code)}, {"message", str(msg)}})}}));
    }
    // MCP tool result: content = [{type:"text", text}], isError on failure.
    [[nodiscard]] static JVal tool_result(const std::string& text, bool is_error) {
        return obj({{"content", arr({obj({{"type", str("text")}, {"text", str(text)}})})},
                    {"isError", boolean(is_error)}});
    }

    // ---- protocol methods ---------------------------------------------------
    [[nodiscard]] std::string initialize_line(const JVal& id) {
        return result_line(
            id, obj({{"protocolVersion", str("2025-06-18")},
                     {"capabilities", obj({{"tools", obj({})}})},
                     {"serverInfo", obj({{"name", str("lockstep-agent-memory")},
                                         {"version", str("0.1")}})}}));
    }

    [[nodiscard]] static JVal tool_decl(const std::string& name, const std::string& desc,
                                        std::vector<JVal::Member> props,
                                        std::vector<JVal> required) {
        return obj({{"name", str(name)},
                    {"description", str(desc)},
                    {"inputSchema", obj({{"type", str("object")},
                                         {"properties", obj(std::move(props))},
                                         {"required", arr(std::move(required))}})}});
    }
    [[nodiscard]] std::string tools_list_line(const JVal& id) {
        const auto sprop = [](const std::string& d) {
            return obj({{"type", str("string")}, {"description", str(d)}});
        };
        const auto nprop = [](const std::string& d) {
            return obj({{"type", str("number")}, {"description", str(d)}});
        };
        const auto aprop = [](const std::string& d) {
            return obj({{"type", str("array")}, {"description", str(d)}});
        };
        return result_line(
            id,
            obj({{"tools",
                  arr({tool_decl("query", "Run a SQL statement; rows return as JSON.",
                                 {{"sql", sprop("the SQL text")}}, {str("sql")}),
                       tool_decl("schema", "List tables and their columns.", {}, {}),
                       tool_decl("remember",
                                 "Store one memory durably (auto-provisions the memory "
                                 "table + vector + BM25 indexes on first use).",
                                 {{"content", sprop("the memory text")},
                                  {"embedding", aprop("optional embedding vector")},
                                  {"kind", sprop("optional tag, e.g. fact/episode")}},
                                 {str("content")}),
                       tool_decl("recall",
                                 "Hybrid recall: RRF-fused vector+BM25 when an embedding "
                                 "is given, BM25 keyword recall otherwise. Deterministic "
                                 "ranking - same memory, same query, same order.",
                                 {{"query", sprop("keyword query text")},
                                  {"embedding", aprop("optional query embedding")},
                                  {"k", nprop("max results (default 5)")}},
                                 {str("query")}),
                       tool_decl("history",
                                 "Run a SELECT as of version n, where n counts committed "
                                 "write statements (each remember = one step) - the "
                                 "agent's exact world at step n (K10 time travel). NOTE: "
                                 "this is the statement-version line, not the CHANGES "
                                 "_seq line.",
                                 {{"sql", sprop("the SELECT")},
                                  {"seq", nprop("the Seq to read at")}},
                                 {str("sql"), str("seq")})})}}));
    }

    // ---- tool dispatch ------------------------------------------------------
    [[nodiscard]] std::string tools_call_line(const JVal& id, const JVal* params) {
        const JVal* name = params ? find(*params, "name") : nullptr;
        const JVal* args = params ? find(*params, "arguments") : nullptr;
        if (name == nullptr || name->kind != JVal::Str) {
            return error_line(id, -32602, "tools/call needs params.name");
        }
        static const JVal kEmpty = [] { JVal v; v.kind = JVal::Obj; return v; }();
        const JVal& a = (args != nullptr && args->kind == JVal::Obj) ? *args : kEmpty;
        // The engine's search_path is ENGINE-global session state: with several agent
        // sessions on one engine, re-pin this agent's schema before every tool call.
        if (!agent_.empty()) {
            (void)eng_->exec("SET search_path TO agent_" + agent_);
        }
        if (name->text == "query") return result_line(id, tool_query(a));
        if (name->text == "schema") return result_line(id, tool_schema());
        if (name->text == "remember") return result_line(id, tool_remember(a));
        if (name->text == "recall") return result_line(id, tool_recall(a));
        if (name->text == "history") return result_line(id, tool_history(a));
        return result_line(id, tool_result("unknown tool '" + name->text + "'", true));
    }

    // Render an ExecResult's rows as a JSON array of objects (deterministic order).
    [[nodiscard]] static JVal rows_json(const sql::ExecResult& r) {
        std::vector<JVal> rows;
        for (const sql::ResultRow& row : r.rows) {
            std::vector<JVal::Member> cells;
            for (const auto& [col, d] : row.cells) {
                cells.push_back({col, str(d.render())});
            }
            rows.push_back(obj(std::move(cells)));
        }
        return arr(std::move(rows));
    }
    [[nodiscard]] JVal exec_to_result(const sql::ExecResult& r) {
        if (!r.ok) return tool_result(r.error, true);
        return tool_result(render(rows_json(r)), false);
    }

    [[nodiscard]] JVal tool_query(const JVal& a) {
        const JVal* q = find(a, "sql");
        if (q == nullptr || q->kind != JVal::Str) return tool_result("missing 'sql'", true);
        return exec_to_result(eng_->exec(q->text));
    }

    [[nodiscard]] JVal tool_schema() {
        const sql::ExecResult tabs = eng_->exec("SHOW TABLES");
        if (!tabs.ok) return tool_result(tabs.error, true);
        std::vector<JVal> out;
        for (const sql::ResultRow& row : tabs.rows) {
            const std::string tname = row.cells[0].second.s;
            const sql::ExecResult cols = eng_->exec("DESCRIBE " + tname);
            std::vector<JVal> cvals;
            if (cols.ok) {
                for (const sql::ResultRow& c : cols.rows) {
                    cvals.push_back(str(c.cells[0].second.render() + " " +
                                        c.cells[1].second.render()));
                }
            }
            out.push_back(obj({{"table", str(tname)}, {"columns", arr(std::move(cvals))}}));
        }
        return tool_result(render(arr(std::move(out))), false);
    }

    // The memory store: provisioned on first remember. With an embedding the schema
    // carries VECTOR(dim) + IVFFLAT + BM25 (hybrid recall); without, TEXT + BM25.
    [[nodiscard]] bool ensure_memory(std::size_t dim, std::string& err) {
        const sql::ExecResult probe = eng_->exec("DESCRIBE agent_memory");
        if (probe.ok) {
            detect_dim(probe);  // a fresh session over an existing store re-learns the dim
            if (dim > 0 && mem_dim_ == 0) {
                err = "memory table exists without embeddings; recall is keyword-only";
                return false;
            }
            if (dim > 0 && dim != mem_dim_) {
                err = "embedding dim " + std::to_string(dim) + " != table dim " +
                      std::to_string(mem_dim_);
                return false;
            }
            return true;
        }
        std::string ddl = "CREATE TABLE agent_memory (id INT AUTO_INCREMENT, kind TEXT, "
                          "content TEXT NOT NULL";
        if (dim > 0) ddl += ", embedding VECTOR(" + std::to_string(dim) + ")";
        ddl += ", PRIMARY KEY (id))";
        if (const sql::ExecResult r = eng_->exec(ddl); !r.ok) {
            err = r.error;
            return false;
        }
        if (const sql::ExecResult r =
                eng_->exec("CREATE INDEX am_txt ON agent_memory (content) USING BM25");
            !r.ok) {
            err = r.error;
            return false;
        }
        if (dim > 0) {
            if (const sql::ExecResult r = eng_->exec(
                    "CREATE INDEX am_vec ON agent_memory (embedding) USING IVFFLAT "
                    "WITH (lists = 8, probes = 8)");
                !r.ok) {
                err = r.error;
                return false;
            }
            mem_dim_ = dim;
        }
        return true;
    }
    // Re-learn the embedding dim from DESCRIBE output (column "embedding VECTOR(d)").
    void detect_dim(const sql::ExecResult& describe) {
        if (mem_dim_ != 0) return;
        for (const sql::ResultRow& c : describe.rows) {
            if (c.cells[0].second.render() != "embedding") continue;
            const std::string ty = c.cells[1].second.render();
            const std::size_t l = ty.find('('), r = ty.find(')');
            if (l != std::string::npos && r != std::string::npos && r > l + 1) {
                mem_dim_ = static_cast<std::size_t>(std::stoul(ty.substr(l + 1, r - l - 1)));
            }
        }
    }
    [[nodiscard]] static std::string sq(const std::string& s) {  // SQL single-quote
        std::string out = "'";
        for (const char c : s) {
            if (c == '\'') out += "''";
            else out += c;
        }
        out += "'";
        return out;
    }
    [[nodiscard]] static std::string vec_literal(const JVal& emb) {
        std::string s = "[";
        for (std::size_t i = 0; i < emb.arr.size(); ++i) {
            if (i != 0) s += ',';
            s += emb.arr[i].text;
        }
        s += ']';
        return s;
    }

    [[nodiscard]] JVal tool_remember(const JVal& a) {
        const JVal* content = find(a, "content");
        if (content == nullptr || content->kind != JVal::Str) {
            return tool_result("missing 'content'", true);
        }
        const JVal* emb = find(a, "embedding");
        const bool has_emb = emb != nullptr && emb->kind == JVal::Arr && !emb->arr.empty();
        std::string err;
        if (!ensure_memory(has_emb ? emb->arr.size() : 0, err)) return tool_result(err, true);
        const JVal* kind = find(a, "kind");
        // Literal SQL (single-quotes doubled): INSERT VALUES takes literals only —
        // $N in VALUES is the recorded WHERE-$N-class open item.
        std::string sql = "INSERT INTO agent_memory (content, kind";
        std::string vals = "(" + sq(content->text) + ", " +
                           sq(kind != nullptr && kind->kind == JVal::Str ? kind->text : "fact");
        if (has_emb) {
            sql += ", embedding";
            vals += ", " + sq(vec_literal(*emb));
        }
        const sql::ExecResult r = eng_->exec(sql + ") VALUES " + vals + ")");
        if (!r.ok) return tool_result(r.error, true);
        return tool_result("remembered (seq cursor advances; recall is immediate)", false);
    }

    [[nodiscard]] JVal tool_recall(const JVal& a) {
        const JVal* q = find(a, "query");
        if (q == nullptr || q->kind != JVal::Str) return tool_result("missing 'query'", true);
        const JVal* kv = find(a, "k");
        const std::string k =
            (kv != nullptr && kv->kind == JVal::Num) ? kv->text : std::string("5");
        if (mem_dim_ == 0) {
            const sql::ExecResult probe = eng_->exec("DESCRIBE agent_memory");
            if (probe.ok) detect_dim(probe);
        }
        const JVal* emb = find(a, "embedding");
        const bool hybrid =
            emb != nullptr && emb->kind == JVal::Arr && !emb->arr.empty() && mem_dim_ > 0;
        // NOTE: literals, not $N — the K1/K2 top-k matchers pattern-match literal
        // arguments (rrf_score/bm25_score in ORDER BY); the strings are quoted here.
        std::string sql;
        if (hybrid) {
            sql = "SELECT id, kind, content FROM agent_memory ORDER BY "
                  "rrf_score(embedding, " + sq(vec_literal(*emb)) + ", content, " +
                  sq(q->text) + ") DESC LIMIT " + k;
        } else {
            sql = "SELECT id, kind, content FROM agent_memory ORDER BY "
                  "bm25_score(content, " + sq(q->text) + ") DESC LIMIT " + k;
        }
        return exec_to_result(eng_->exec(sql));
    }

    [[nodiscard]] JVal tool_history(const JVal& a) {
        const JVal* q = find(a, "sql");
        const JVal* seq = find(a, "seq");
        if (q == nullptr || q->kind != JVal::Str || seq == nullptr || seq->kind != JVal::Num) {
            return tool_result("history needs 'sql' and 'seq'", true);
        }
        return exec_to_result(eng_->exec(q->text + " AS OF SEQ " + seq->text));
    }

    sql::SqlEngine* eng_;
    std::string agent_;
    std::size_t mem_dim_ = 0;  // embedding dim once provisioned (0 = text-only store)
};

}  // namespace lockstep::query::mcp
