// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// mcp_server.h
//
// In-process MCP server. Runs its network I/O on a worker thread so REAPER is never blocked.
// Primary transport: Streamable HTTP bound to 127.0.0.1:<port> with a bearer token.
// Every tools/call handler is dispatched onto the main thread via MainThreadQueue.
//
// Target spec: MCP 2025-06-18 (tools, resources, prompts, structured output). Resource
// subscriptions ride the SSE channel: resources/subscribe|unsubscribe update a SubscriptionHub, the
// control surface marks changed URIs dirty, and the GET /mcp SSE stream flushes
// `notifications/resources/updated` for them.
//
// The HTTP/JSON deps (cpp-httplib, nlohmann/json) stay in the .cpp — this header is dep-free so the
// SDK/entry TU doesn't pull them.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "elicitation.h"
#include "main_thread_queue.h"
#include "prompt_registry.h"
#include "resource_registry.h"
#include "subscription_hub.h"
#include "tool_registry.h"

// fwd-decls; concrete httplib types live in mcp_server.cpp. Response is used by-reference only in the
// elicitation call lane (which answers a POST with an SSE stream), so a fwd-decl suffices here.
namespace httplib { class Server; struct Response; }

namespace reaper_mcp {

struct ServerConfig {
    std::string host = "127.0.0.1";  // localhost only — never bind a public interface
    uint16_t port = 0;               // 0 = pick a free port and print it to the REAPER console
    std::string token;               // bearer token; generated at startup
    bool enableStdioShim = false;    // spawn a stdio<->HTTP shim for stdio-only clients
    int callTimeoutMs = 5000;        // cap on a tools/call awaiting the main-thread pump
    // A tools/call that elicits is answered on an SSE stream and awaits a HUMAN off the main thread —
    // the 5 s callTimeoutMs must NOT apply. This is the (much longer) cap on that wait; on expiry the
    // call aborts with NO mutation.
    int elicitationTimeoutMs = 120000;
};

class McpServer {
public:
    McpServer(ServerConfig cfg, ToolRegistry& tools, ResourceRegistry& resources,
              PromptRegistry& prompts, SubscriptionHub& subs, MainThreadQueue& queue);
    ~McpServer();

    void start();  // binds synchronously (so boundPort() is valid on return), then serves on a worker
    void stop();   // signals shutdown, stops the listener, joins

    uint16_t boundPort() const { return cfg_.port; }
    const std::string& token() const { return cfg_.token; }
    const std::string& sessionId() const { return sessionId_; }
    bool isRunning() const { return running_.load(); }

private:
    void setupRoutes();

    // JSON-RPC 2.0 routing. dispatch() returns the response object, or a null Json for notifications
    // (which must not be answered).
    Json dispatch(const Json& msg);
    Json handleInitialize(const Json& params);
    Json handleToolsList(const Json& params);
    Json handleToolsCall(const Json& id, const Json& params);
    // The single-message tools/call lane with access to the HTTP response, so a call that
    // elicits can switch its answer to an SSE stream (validate -> elicitation/create -> await off the
    // main thread -> re-marshal the mutation -> stream the result). Non-eliciting calls set a plain
    // JSON body, identical to handleToolsCall.
    void handleToolsCallHttp(const Json& id, const Json& params, httplib::Response& res,
                             bool elicitCapable);
    Json handleResourcesList(const Json& params);
    Json handleResourcesTemplatesList(const Json& params);
    Json handleResourcesRead(const Json& id, const Json& params);
    Json handleResourcesSubscribe(const Json& id, const Json& params, bool subscribe);
    Json handlePromptsList(const Json& params);
    Json handlePromptsGet(const Json& id, const Json& params);

    ServerConfig cfg_;
    ToolRegistry& tools_;
    ResourceRegistry& resources_;
    PromptRegistry& prompts_;
    SubscriptionHub& subs_;
    MainThreadQueue& queue_;

    std::unique_ptr<httplib::Server> svr_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::string sessionId_;
    std::mutex sessionMutex_;
    std::vector<Profile> activeProfiles_{Profile::Full};  // negotiated at initialize

    // MCP elicitation. clientElicitation_ records whether the client advertised
    // `capabilities.elicitation` at initialize (the only client capability the server inspects).
    // elicitations_ correlates a streamed `elicitation/create` request with the client's follow-up
    // response POST, keyed by the server-generated JSON-RPC id.
    std::atomic<bool> clientElicitation_{false};
    ElicitationRegistry elicitations_;
};

}  // namespace reaper_mcp
