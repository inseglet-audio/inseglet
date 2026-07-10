// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// mcp_server.cpp — HTTP / JSON-RPC transport for the MCP server.
//
// Streamable HTTP (cpp-httplib) + JSON-RPC 2.0 (nlohmann/json) implementing the MCP 2025-06-18
// server methods: initialize, ping, tools/list, tools/call, resources/list, resources/templates/list,
// resources/read, resources/subscribe, resources/unsubscribe, prompts/list, prompts/get, and an
// SSE channel (GET /mcp) that pushes server->client notifications/resources/updated for subscribed
// resources and carries the elicitation/create round-trip for destructive tools/call.
//
// The critical invariant: NEVER call a REAPER API function from a request-handler thread. Tool
// bodies run on the main thread via queue_.submit([...]).get(). We copy `arguments` into the task so
// a timed-out call can never dangle references (the task may still run after we stop waiting).

#include "mcp_server.h"

#include <chrono>
#include <cstring>
#include <random>
#include <string>

// cpp-httplib is plain HTTP for loopback use — no OpenSSL/zlib link required.
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace reaper_mcp {
namespace {

constexpr char kProtocolVersion[] = "2025-06-18";
constexpr char kServerName[] = "reaper_mcp";
constexpr char kServerVersion[] = "1.5.0";

std::string randomHex(int bytes) {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ rd());
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(static_cast<size_t>(bytes) * 2);
    for (int i = 0; i < bytes * 2; ++i) s.push_back(hex[dist(gen)]);
    return s;
}

Json rpcResult(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}
Json rpcError(const Json& id, int code, const std::string& message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

// One SSE frame carrying a JSON-RPC `notifications/resources/updated` for a changed resource. Per
// MCP Streamable HTTP, a server->client message is the JSON in the SSE `data:` field; a blank line
// terminates the event. nlohmann handles any escaping the URI would need.
std::string resourceUpdatedFrame(const std::string& uri) {
    const Json n = {{"jsonrpc", "2.0"},
                    {"method", "notifications/resources/updated"},
                    {"params", {{"uri", uri}}}};
    return "data: " + n.dump() + "\n\n";
}

// --- elicitation helpers ---------------------------------------------------------------------

// Frame an arbitrary JSON-RPC message as one SSE `data:` event.
std::string sseData(const Json& msg) { return "data: " + msg.dump() + "\n\n"; }

// The two shapes of a tools/call `result` object (structured success vs isError), factored so the
// normal lane and the elicitation lane build identical envelopes.
Json toolResultOk(const Json& structured) {
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", structured.dump()}}})},
                {"structuredContent", structured},
                {"isError", false}};
}
Json toolResultError(const std::string& text) {
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})}, {"isError", true}};
}

// A client->server JSON-RPC RESPONSE (e.g. an `elicitation/create` answer): carries an id, no method,
// and a result or error member. These are correlated to a waiting stream, never dispatched.
bool isClientResponse(const Json& m) {
    return m.is_object() && m.contains("id") && !m.contains("method") &&
           (m.contains("result") || m.contains("error"));
}

// Stable string key for a JSON-RPC id (elicitation ids we mint are strings; be tolerant of numbers).
std::string idKey(const Json& id) {
    return id.is_string() ? id.get<std::string>() : id.dump();
}

}  // namespace

McpServer::McpServer(ServerConfig cfg, ToolRegistry& tools, ResourceRegistry& resources,
                     PromptRegistry& prompts, SubscriptionHub& subs, MainThreadQueue& queue)
    : cfg_(std::move(cfg)),
      tools_(tools),
      resources_(resources),
      prompts_(prompts),
      subs_(subs),
      queue_(queue),
      sessionId_(randomHex(16)) {}

McpServer::~McpServer() { stop(); }

// ---------------------------------------------------------------------------------------------
// JSON-RPC method handlers
// ---------------------------------------------------------------------------------------------

Json McpServer::handleInitialize(const Json& params) {
    // Optional non-standard extension: let a client request a bounded profile set to stay under
    // tool-count caps, e.g. {"profiles":["core","spatial"]}. Default = full surface.
    std::vector<Profile> profiles;
    if (params.contains("profiles") && params["profiles"].is_array()) {
        for (const auto& p : params["profiles"]) {
            Profile parsed;
            if (p.is_string() && parseProfile(p.get<std::string>(), parsed)) profiles.push_back(parsed);
        }
    }
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        activeProfiles_ = profiles.empty() ? std::vector<Profile>{Profile::Full} : profiles;
    }

    // Record whether the client can honor a server->client `elicitation/create`. MCP puts this in the
    // CLIENT's `capabilities.elicitation` (presence of the key = supported); this is the only client
    // capability the server inspects. A destructive verb consults it to pick the elicitation round-trip
    // vs the `confirm:true` fallback. (elicitation is a client capability — the server advertises
    // nothing new below.)
    bool elic = false;
    if (params.contains("capabilities") && params["capabilities"].is_object())
        elic = params["capabilities"].contains("elicitation");
    clientElicitation_.store(elic);

    return Json{
        {"protocolVersion", kProtocolVersion},
        {"capabilities",
         {{"tools", {{"listChanged", false}}},
          {"resources", {{"listChanged", false}, {"subscribe", true}}},
          {"prompts", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
        {"instructions",
         "REAPER control via MCP. Read-only tools (transport.get_state, project.get_summary, "
         "track.list, track.get_name) are safe to call freely; mutating tools are single-undo. "
         "Spatial/immersive tools live under the 'spatial' profile."}};
}

Json McpServer::handleToolsList(const Json& /*params*/) {
    std::vector<Profile> active;
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        active = activeProfiles_;
    }
    Json tools = Json::array();
    for (const Tool* t : tools_.list(active)) {
        Json j{
            {"name", t->name},
            {"description", t->description},
            {"inputSchema", t->inputSchema},
            {"annotations",
             {{"readOnlyHint", t->annotations.readOnly},
              {"destructiveHint", t->annotations.destructive},
              {"idempotentHint", t->annotations.idempotent}}},
        };
        if (!t->outputSchema.is_null()) j["outputSchema"] = t->outputSchema;
        tools.push_back(std::move(j));
    }
    return Json{{"tools", std::move(tools)}};
}

// The batch / non-elicitation tools/call path. A destructive verb here runs WITHOUT an elicitation
// scope, so requireElicitation() no-ops and the verb applies its confirm:true stopgap — elicitation
// is offered only for a single (non-batch) tools/call, which owns its HTTP response (see
// handleToolsCallHttp). MCP clients send a destructive call as a single request anyway.
Json McpServer::handleToolsCall(const Json& id, const Json& params) {
    const std::string name = params.value("name", std::string());
    const Json args = params.contains("arguments") ? params["arguments"] : Json::object();

    const Tool* t = tools_.find(name);
    if (!t) return rpcError(id, -32602, "Unknown tool: " + name);

    bool isError = false;
    std::string errText;
    Json structured;
    try {
        // Copy args into the task: if we time out below, the queued closure must not reference
        // stack locals that have gone out of scope.
        auto fut = queue_.submit([t, args]() -> Json { return t->handler(args); });
        if (fut.wait_for(std::chrono::milliseconds(cfg_.callTimeoutMs)) == std::future_status::ready) {
            structured = fut.get();
        } else {
            isError = true;
            errText = "Tool call timed out after " + std::to_string(cfg_.callTimeoutMs) +
                      "ms (is REAPER's main thread pumping Run()?)";
        }
    } catch (const std::exception& e) {
        isError = true;
        errText = std::string("Tool handler threw: ") + e.what();
    }

    Json result;
    if (isError) {
        result = Json{{"content", Json::array({Json{{"type", "text"}, {"text", errText}}})},
                      {"isError", true}};
    } else {
        result = Json{{"content", Json::array({Json{{"type", "text"}, {"text", structured.dump()}}})},
                      {"structuredContent", structured},
                      {"isError", false}};
    }
    return rpcResult(id, result);
}

// The single-message tools/call lane — spec-faithful async elicitation.
//
// Two phases:
//   VALIDATE — run the handler on the main thread with an ElicitationScope reflecting the client's
//     capability. A non-eliciting call (any read/mutate verb, or a destructive verb given confirm:true
//     or targeting an empty project) returns a value → we set a normal JSON body, byte-for-byte like
//     handleToolsCall. A destructive verb that needs the human's OK throws ElicitationRequest BEFORE
//     opening its UndoTransaction (zero mutation), which fut.get() re-throws here.
//   ELICIT — answer THIS POST with an SSE stream: emit `elicitation/create`, AWAIT the client's
//     response OFF the main thread (this httplib connection thread — REAPER keeps pumping Run()),
//     enforce elicitationTimeoutMs, then on accept re-marshal the MUTATION to the main-thread queue
//     with confirm:true folded in and stream the tools/call result; on decline/cancel/timeout stream
//     an isError result having mutated nothing.
void McpServer::handleToolsCallHttp(const Json& id, const Json& params, httplib::Response& res,
                                    bool elicitCapable) {
    const std::string name = params.value("name", std::string());
    const Json args = params.contains("arguments") ? params["arguments"] : Json::object();

    const Tool* t = tools_.find(name);
    if (!t) {
        res.set_content(rpcError(id, -32602, "Unknown tool: " + name).dump(), "application/json");
        return;
    }

    // --- VALIDATE (main thread) ---
    Json structured;
    bool isError = false;
    std::string errText;
    bool needElicit = false;
    ElicitationRequest elicReq;
    try {
        auto fut = queue_.submit([t, args, elicitCapable]() -> Json {
            ElicitationScope scope(elicitCapable);  // set on the MAIN thread; requireElicitation reads it
            return t->handler(args);
        });
        if (fut.wait_for(std::chrono::milliseconds(cfg_.callTimeoutMs)) == std::future_status::ready) {
            structured = fut.get();  // may re-throw ElicitationRequest (checked first) or std::exception
        } else {
            isError = true;
            errText = "Tool call timed out after " + std::to_string(cfg_.callTimeoutMs) +
                      "ms (is REAPER's main thread pumping Run()?)";
        }
    } catch (const ElicitationRequest& er) {
        needElicit = true;
        elicReq = er;
    } catch (const std::exception& e) {
        isError = true;
        errText = std::string("Tool handler threw: ") + e.what();
    }

    if (!needElicit) {
        const Json result = isError ? toolResultError(errText) : toolResultOk(structured);
        res.set_content(rpcResult(id, result).dump(), "application/json");
        return;
    }

    // --- ELICIT (SSE-answered) ---
    const std::string elicId = "elic-" + randomHex(8);
    elicitations_.create(elicId);
    const Json elicCreate =
        Json{{"jsonrpc", "2.0"},
             {"id", elicId},
             {"method", "elicitation/create"},
             {"params", {{"message", elicReq.message}, {"requestedSchema", elicReq.requestedSchema}}}};

    struct StreamState {
        int phase = 0;
        std::chrono::steady_clock::time_point start;
    };
    auto stt = std::make_shared<StreamState>();

    res.set_chunked_content_provider(
        "text/event-stream",
        [this, id, t, args, elicId, elicCreate, stt](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            if (stt->phase == 0) {
                const std::string frame = sseData(elicCreate);
                if (!sink.write(frame.data(), frame.size())) {
                    elicitations_.remove(elicId);
                    return false;
                }
                stt->phase = 1;
                stt->start = std::chrono::steady_clock::now();
                return true;
            }

            // phase 1: await the answer in short slices so we can also enforce the overall cap and keep
            // the stream alive with a heartbeat comment.
            ElicitationOutcome out;
            if (!elicitations_.poll(elicId, std::chrono::milliseconds(200), out)) {
                if (!running_.load()) {
                    elicitations_.remove(elicId);
                    return false;  // shutting down
                }
                const auto elapsed = std::chrono::steady_clock::now() - stt->start;
                if (elapsed >= std::chrono::milliseconds(cfg_.elicitationTimeoutMs)) {
                    elicitations_.remove(elicId);
                    const Json result = toolResultError(
                        "Elicitation timed out after " + std::to_string(cfg_.elicitationTimeoutMs) +
                        "ms with no client response; no changes were made.");
                    const std::string frame = sseData(rpcResult(id, result));
                    sink.write(frame.data(), frame.size());
                    return false;
                }
                static const char kBeat[] = ": waiting-for-elicitation\n\n";  // SSE comment; ignored
                return sink.write(kBeat, sizeof(kBeat) - 1);
            }

            // terminal — settle the slot and build the tools/call result.
            elicitations_.remove(elicId);
            Json result;
            bool proceed = false;
            Json folded;  // elicited content to fold into args on accept
            if (out.kind == ElicitationOutcome::Answered && !out.response.contains("error")) {
                const Json er = (out.response.contains("result") && out.response["result"].is_object())
                                    ? out.response["result"]
                                    : Json::object();
                const std::string action = er.value("action", std::string());
                const Json content =
                    (er.contains("content") && er["content"].is_object()) ? er["content"] : Json::object();
                proceed = (action == "accept") &&
                          (!content.contains("confirm") || content.value("confirm", true));
                if (proceed) folded = content;
                else if (action == "decline")
                    result = toolResultError("Elicitation declined by the user; no changes were made.");
                else
                    result = toolResultError("Elicitation cancelled; no changes were made.");
            } else if (out.kind == ElicitationOutcome::Answered) {  // response carried an error
                result = toolResultError("Elicitation failed on the client; no changes were made.");
            } else {  // Cancelled
                result = toolResultError("Elicitation cancelled; no changes were made.");
            }

            if (proceed) {
                // Re-marshal the MUTATION to the main thread with confirm:true (+ any elicited fields)
                // folded in. No elicitation scope this time, so requireElicitation() no-ops and the
                // verb's own confirmation gate is satisfied.
                Json args2 = args;
                for (auto it = folded.begin(); it != folded.end(); ++it) args2[it.key()] = it.value();
                args2["confirm"] = true;
                try {
                    auto fut = queue_.submit([t, args2]() -> Json { return t->handler(args2); });
                    if (fut.wait_for(std::chrono::milliseconds(cfg_.callTimeoutMs)) ==
                        std::future_status::ready)
                        result = toolResultOk(fut.get());
                    else
                        result = toolResultError("Tool mutation timed out after the elicitation was accepted.");
                } catch (const ElicitationRequest&) {
                    result = toolResultError("Unexpected re-elicitation after confirmation; aborted.");
                } catch (const std::exception& e) {
                    result = toolResultError(std::string("Tool handler threw after elicitation: ") + e.what());
                }
            }

            const std::string frame = sseData(rpcResult(id, result));
            sink.write(frame.data(), frame.size());
            return false;  // end the stream
        });
}

Json McpServer::handleResourcesList(const Json& /*params*/) {
    Json arr = Json::array();
    for (const Resource& r : resources_.all()) {
        if (r.isTemplate) continue;  // templates are advertised via resources/templates/list
        arr.push_back(Json{{"uri", r.uri},
                           {"name", r.name},
                           {"description", r.description},
                           {"mimeType", r.mimeType}});
    }
    return Json{{"resources", std::move(arr)}};
}

Json McpServer::handleResourcesTemplatesList(const Json& /*params*/) {
    Json arr = Json::array();
    for (const Resource& r : resources_.all()) {
        if (!r.isTemplate) continue;
        arr.push_back(Json{{"uriTemplate", r.uri},
                           {"name", r.name},
                           {"description", r.description},
                           {"mimeType", r.mimeType}});
    }
    return Json{{"resourceTemplates", std::move(arr)}};
}

Json McpServer::handleResourcesRead(const Json& id, const Json& params) {
    const std::string uri = params.value("uri", std::string());
    if (uri.empty()) return rpcError(id, -32602, "resources/read requires a 'uri' parameter");

    const Resource* r = resources_.resolve(uri);
    if (!r) return rpcError(id, -32602, "Unknown resource: " + uri);

    // The reader touches the REAPER API, so it must run on the main thread — same hop as tools/call.
    // Copy what the closure needs (the reader fn + uri) so a timed-out call can't dangle references.
    auto reader = r->read;
    const std::string mimeType = r->mimeType;
    try {
        auto fut = queue_.submit([reader, uri]() -> std::string { return reader(uri); });
        if (fut.wait_for(std::chrono::milliseconds(cfg_.callTimeoutMs)) != std::future_status::ready)
            return rpcError(id, -32000,
                            "Resource read timed out after " + std::to_string(cfg_.callTimeoutMs) +
                                "ms (is REAPER's main thread pumping Run()?)");
        const std::string text = fut.get();
        Json contents = Json::array(
            {Json{{"uri", uri}, {"mimeType", mimeType}, {"text", text}}});
        return rpcResult(id, Json{{"contents", std::move(contents)}});
    } catch (const std::exception& e) {
        return rpcError(id, -32000, std::string("Resource read failed: ") + e.what());
    }
}

// resources/subscribe and resources/unsubscribe. Both take {"uri": ...} and return an empty result.
// The actual push happens on the SSE channel: subscribe() records the URI in the hub; the control
// surface marks it dirty when REAPER reports a matching change; the GET /mcp stream flushes
// notifications/resources/updated. A subscribe to a URI that matches a resource *template*
// (reaper://track/{index}/chunk) is honored as an exact-URI watch (e.g. reaper://track/0/chunk).
Json McpServer::handleResourcesSubscribe(const Json& id, const Json& params, bool subscribe) {
    const std::string uri = params.value("uri", std::string());
    if (uri.empty())
        return rpcError(id, -32602,
                        std::string("resources/") + (subscribe ? "subscribe" : "unsubscribe") +
                            " requires a 'uri' parameter");
    if (subscribe) {
        // Only allow subscribing to something we can actually serve (a static resource or a concrete
        // instance of a template), so a client gets an early error instead of silent no-updates.
        if (!resources_.resolve(uri))
            return rpcError(id, -32602, "Unknown resource: " + uri);
        subs_.subscribe(uri);
    } else {
        subs_.unsubscribe(uri);
    }
    return rpcResult(id, Json::object());
}

// prompts/list — the expert-workflow templates. Pure data (no REAPER API), so
// unlike tools/resources it does not hop the main thread.
Json McpServer::handlePromptsList(const Json& /*params*/) { return prompts_.listJson(); }

// prompts/get — render a named prompt with its (string) arguments. Validates required arguments so a
// client gets a clear -32602 instead of a half-rendered template. render() is deterministic + REAPER-
// free, so it runs inline on the request thread.
Json McpServer::handlePromptsGet(const Json& id, const Json& params) {
    const std::string name = params.value("name", std::string());
    if (name.empty()) return rpcError(id, -32602, "prompts/get requires a 'name' parameter");

    const Prompt* p = prompts_.find(name);
    if (!p) return rpcError(id, -32602, "Unknown prompt: " + name);

    const Json args =
        (params.contains("arguments") && params["arguments"].is_object()) ? params["arguments"]
                                                                          : Json::object();
    for (const auto& a : p->arguments) {
        if (a.required && (!args.contains(a.name) || !args[a.name].is_string() ||
                           args[a.name].get<std::string>().empty()))
            return rpcError(id, -32602, "prompts/get '" + name + "' missing required argument: " + a.name);
    }
    try {
        return rpcResult(id, PromptRegistry::getJson(*p, args));
    } catch (const std::exception& e) {
        return rpcError(id, -32603, std::string("prompt render failed: ") + e.what());
    }
}

Json McpServer::dispatch(const Json& msg) {
    if (!msg.is_object()) return rpcError(Json(), -32600, "Invalid Request");

    const bool isNotification = !msg.contains("id");
    const Json id = msg.contains("id") ? msg["id"] : Json();
    const std::string method = msg.value("method", std::string());
    const Json params = msg.contains("params") ? msg["params"] : Json::object();

    // Notifications (client -> server, no reply). We accept and drop.
    if (isNotification) return Json();

    if (method.empty()) return rpcError(id, -32600, "Invalid Request: missing method");

    if (method == "initialize") return rpcResult(id, handleInitialize(params));
    if (method == "ping") return rpcResult(id, Json::object());
    if (method == "tools/list") return rpcResult(id, handleToolsList(params));
    if (method == "tools/call") return handleToolsCall(id, params);
    if (method == "resources/list") return rpcResult(id, handleResourcesList(params));
    if (method == "resources/templates/list")
        return rpcResult(id, handleResourcesTemplatesList(params));
    if (method == "resources/read") return handleResourcesRead(id, params);
    // Resource subscriptions. subscribe/unsubscribe update the SubscriptionHub; the SSE
    // channel (GET /mcp) delivers notifications/resources/updated as the control surface marks URIs
    // dirty. capabilities.resources.subscribe is advertised true at initialize.
    if (method == "resources/subscribe") return handleResourcesSubscribe(id, params, /*subscribe*/ true);
    if (method == "resources/unsubscribe") return handleResourcesSubscribe(id, params, /*subscribe*/ false);
    // MCP prompts: declarative expert workflows. capabilities.prompts is advertised at
    // initialize; prompts/list + prompts/get return the templates from the PromptRegistry.
    if (method == "prompts/list") return rpcResult(id, handlePromptsList(params));
    if (method == "prompts/get") return handlePromptsGet(id, params);

    return rpcError(id, -32601, "Method not found: " + method);
}

// ---------------------------------------------------------------------------------------------
// HTTP wiring
// ---------------------------------------------------------------------------------------------

void McpServer::setupRoutes() {
    svr_->set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Mcp-Session-Id", sessionId_},
    });

    auto authOk = [this](const httplib::Request& req) -> bool {
        if (cfg_.token.empty()) return true;  // no token configured (dev) -> open on loopback
        return req.get_header_value("Authorization") == ("Bearer " + cfg_.token);
    };

    // CORS preflight — never requires auth.
    svr_->Options("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    // Client -> server JSON-RPC (single message or a batch array).
    svr_->Post("/mcp", [this, authOk](const httplib::Request& req, httplib::Response& res) {
        if (!authOk(req)) {
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer");
            res.set_content(rpcError(Json(), -32001, "Unauthorized").dump(), "application/json");
            return;
        }
        Json body = Json::parse(req.body, nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded()) {
            res.set_content(rpcError(Json(), -32700, "Parse error").dump(), "application/json");
            return;
        }
        if (body.is_array()) {
            Json out = Json::array();
            for (const auto& m : body) {
                // A client->server RESPONSE (e.g. an elicitation answer) is correlated to
                // a waiting stream, not dispatched. (A tools/call inside a batch can't be answered with
                // SSE, so it never elicits — see handleToolsCall.)
                if (isClientResponse(m)) {
                    elicitations_.deliver(idKey(m["id"]), m);
                    continue;
                }
                Json r = dispatch(m);
                if (!r.is_null()) out.push_back(std::move(r));
            }
            if (out.empty()) {
                res.status = 202;  // batch of notifications / correlated responses only
                return;
            }
            res.set_content(out.dump(), "application/json");
            return;
        }

        // --- single message ---
        // A client->server RESPONSE (an elicitation answer) is delivered to its waiting
        // stream; there is no reply body.
        if (isClientResponse(body)) {
            elicitations_.deliver(idKey(body["id"]), body);
            res.status = 202;
            return;
        }
        // A single tools/call gets the HTTP-aware lane so it can switch its answer to an
        // SSE stream and elicit. Everything else stays on the synchronous dispatch path.
        if (body.is_object() && body.contains("id") &&
            body.value("method", std::string()) == "tools/call") {
            handleToolsCallHttp(body["id"], body.contains("params") ? body["params"] : Json::object(),
                                res, clientElicitation_.load());
            return;
        }
        Json out = dispatch(body);
        if (out.is_null()) {
            res.status = 202;  // a lone notification
            return;
        }
        res.set_content(out.dump(), "application/json");
    });

    // Server -> client stream (SSE). Delivers notifications/resources/updated for subscribed
    // resources as REAPER reports changes, and a periodic heartbeat when idle.
    //
    // Single-consumer model: drainUpdated() clears the hub's pending set, so if more than one SSE
    // stream is open they'd split the updates between them. A localhost single-session MCP server
    // normally holds exactly one GET stream, which is what we target; fanning out to N concurrent
    // subscribers would need per-connection queues (not implemented here).
    svr_->Get("/mcp", [this, authOk](const httplib::Request& req, httplib::Response& res) {
        if (!authOk(req)) {
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer");
            return;
        }
        res.set_chunked_content_provider(
            "text/event-stream",
            [this](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                // Each invocation runs one ~10s cycle: poll the hub every ~100ms and flush any
                // updates the moment a subscribed resource changes, then emit a heartbeat so the
                // channel (and any intermediary proxies) stay alive during quiet periods. Returning
                // false ends the stream — we do so promptly once running_ is cleared at shutdown.
                constexpr int kPollMs = 100;
                constexpr int kPollsPerHeartbeat = 100;  // ~10s between heartbeats
                for (int i = 0; i < kPollsPerHeartbeat; ++i) {
                    if (!running_.load()) return false;
                    for (const std::string& uri : subs_.drainUpdated()) {
                        const std::string frame = resourceUpdatedFrame(uri);
                        if (!sink.write(frame.data(), frame.size())) return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                }
                if (!running_.load()) return false;
                static const char kBeat[] = ": heartbeat\n\n";
                return sink.write(kBeat, std::strlen(kBeat));
            });
    });
}

void McpServer::start() {
    if (running_.load()) return;
    svr_ = std::make_unique<httplib::Server>();
    setupRoutes();

    // Bind synchronously so boundPort() is valid the moment start() returns. port 0 => OS-assigned.
    int port = svr_->bind_to_any_port(cfg_.host.c_str(), cfg_.port);
    if (port <= 0) {
        svr_.reset();
        return;  // bind failed; boundPort() stays 0
    }
    cfg_.port = static_cast<uint16_t>(port);
    running_.store(true);
    worker_ = std::thread([this] { svr_->listen_after_bind(); });
}

void McpServer::stop() {
    if (!running_.exchange(false)) return;
    if (svr_) svr_->stop();
    if (worker_.joinable()) worker_.join();
    svr_.reset();
}

}  // namespace reaper_mcp
