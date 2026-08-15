#include "TestHarness.h"
#include "server/BaaLanguageServer.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace {
class MessageCapture
{
public:
    void append(std::string body)
    {
        std::scoped_lock lock(m_mutex);
        m_messages.push_back(Json::parse(body));
    }

    std::vector<Json> snapshot()
    {
        std::scoped_lock lock(m_mutex);
        return m_messages;
    }

    void clear()
    {
        std::scoped_lock lock(m_mutex);
        m_messages.clear();
    }

private:
    std::mutex m_mutex;
    std::vector<Json> m_messages;
};

const Json *responseWithId(const std::vector<Json> &messages, int id)
{
    const auto found = std::ranges::find_if(messages, [id](const Json &message) {
        return message.value("id", Json(nullptr)) == id;
    });
    return found == messages.end() ? nullptr : &*found;
}
}

int main()
{
    MessageCapture standardCapture;
    BaaLanguageServer standardServer;
    int requestedExit = -1;
    standardServer.setMessageCallback([&standardCapture](std::string body) {
        standardCapture.append(std::move(body));
    });
    standardServer.setExitCallback(
        [&requestedExit](int code) { requestedExit = code; });

    standardServer.receiveMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"shutdown"})");
    std::vector<Json> messages = standardCapture.snapshot();
    CHECK(messages.size() == 1);
    CHECK(messages.back()["error"]["code"] == -32002);

    standardCapture.clear();
    standardServer.receiveMessage(
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})");
    messages = standardCapture.snapshot();
    const Json *initialize = responseWithId(messages, 2);
    CHECK(initialize != nullptr);
    const Json &capabilities = (*initialize)["result"]["capabilities"];
    CHECK(capabilities["positionEncoding"] == "utf-16");
    CHECK(capabilities.contains("textDocumentSync"));
    CHECK(capabilities["documentSymbolProvider"] == true);
    CHECK(capabilities["foldingRangeProvider"] == true);
    CHECK(capabilities["selectionRangeProvider"] == true);
    CHECK(capabilities["inlayHintProvider"] == true);
    CHECK(capabilities["semanticTokensProvider"]["full"] == true);
    CHECK(capabilities["semanticTokensProvider"]["legend"]["tokenTypes"] ==
          Json::array({"type", "macro", "keyword", "modifier", "comment",
                       "string", "number", "operator", "function", "variable",
                       "parameter", "property", "enumMember"}));
    CHECK(capabilities["workspaceSymbolProvider"] == true);
    CHECK(capabilities["workspace"]["workspaceFolders"]["supported"] == true);
    CHECK(capabilities["workspace"]["workspaceFolders"]
                      ["changeNotifications"] == true);
    CHECK(capabilities["hoverProvider"] == true);
    CHECK(capabilities["definitionProvider"] == true);
    CHECK(capabilities["referencesProvider"] == true);
    CHECK(capabilities["documentFormattingProvider"] == true);
    CHECK(capabilities["renameProvider"]["prepareProvider"] == true);
    CHECK(capabilities.contains("signatureHelpProvider"));
    CHECK(capabilities.contains("completionProvider"));
    CHECK(capabilities["experimental"]["baaLogEvent"]["schemaVersion"] ==
          "baa-lsp-log-v1");
    CHECK(capabilities["experimental"]["baaLogEvent"]["transport"] ==
          "local-stdio");
    CHECK(capabilities["experimental"]["baaLogEvent"]["telemetry"] == false);
    const Json triggers = capabilities["completionProvider"]["triggerCharacters"];
    CHECK(std::ranges::find(triggers, Json("ا")) != triggers.end());
    CHECK(std::ranges::find(triggers, Json("#")) != triggers.end());

    standardServer.receiveMessage(
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    standardServer.receiveMessage(
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///missing.baa","version":1},"contentChanges":[{}]}})");
    messages = standardCapture.snapshot();
    CHECK(std::ranges::any_of(messages, [](const Json &message) {
        return message.value("method", "") == "window/logMessage" and
            message.value("params", Json::object()).value("type", 0) == 1;
    }));
    CHECK(std::ranges::none_of(messages, [](const Json &message) {
        return message.value("method", "") == "baa/logEvent" or
            message.value("method", "") == "telemetry/event";
    }));

    standardServer.receiveMessage(
        R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":null})");
    messages = standardCapture.snapshot();
    const Json *shutdown = responseWithId(messages, 3);
    CHECK(shutdown != nullptr);
    CHECK((*shutdown)["result"].is_null());
    standardServer.receiveMessage(
        R"({"jsonrpc":"2.0","method":"exit","params":null})");
    CHECK(requestedExit == 0);

    MessageCapture structuredCapture;
    BaaLanguageServer structuredServer;
    structuredServer.setMessageCallback([&structuredCapture](std::string body) {
        structuredCapture.append(std::move(body));
    });
    structuredServer.receiveMessage(
        R"({"jsonrpc":"2.0","id":10,"method":"initialize","params":{"initializationOptions":{"baaStructuredLogs":{"schemaVersion":"baa-lsp-log-v1"}}}})");
    structuredServer.receiveMessage(
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    for (int version = 1; version <= 2; ++version) {
        structuredServer.receiveMessage(
            std::string("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///missing.baa\","
                        "\"version\":") + std::to_string(version) +
            "},\"contentChanges\":[{}]}}");
    }

    messages = structuredCapture.snapshot();
    std::uint64_t previousSequence = 0;
    int invalidChangeEvents = 0;
    for (const Json &message : messages) {
        CHECK(message.value("method", "") != "telemetry/event");
        CHECK(message.value("method", "") != "window/logMessage");
        if (message.value("method", "") != "baa/logEvent") continue;
        const Json &params = message["params"];
        CHECK(params.size() == 7);
        CHECK(params["schema_version"] == "baa-lsp-log-v1");
        CHECK(params["sequence"].is_number_unsigned());
        const std::uint64_t sequence = params["sequence"].get<std::uint64_t>();
        CHECK(sequence > previousSequence);
        previousSequence = sequence;
        CHECK(params["severity"].is_string());
        CHECK(params["component"].is_string());
        CHECK(params["event"].is_string());
        CHECK(params["message"].is_string());
        CHECK(params["data"].is_object());
        if (params["event"] == "document.change.invalid") {
            ++invalidChangeEvents;
            CHECK(params["component"] == "document");
            CHECK(params["severity"] == "error");
        }
    }
    CHECK(invalidChangeEvents == 2);

    structuredServer.receiveMessage(
        R"({"jsonrpc":"2.0","id":11,"method":"shutdown","params":null})");
    structuredServer.receiveMessage(
        R"({"jsonrpc":"2.0","method":"exit","params":null})");
    return 0;
}
