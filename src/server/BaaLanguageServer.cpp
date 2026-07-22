#include "server/BaaLanguageServer.h"

#include "lsp/PositionEncoding.h"

#include <charconv>

namespace {
constexpr int ParseError = -32700;
constexpr int InvalidRequest = -32600;
constexpr int MethodNotFound = -32601;
constexpr int ServerNotInitialized = -32002;

std::string stringValue(const Json &object, std::string_view key)
{
    const auto it = object.find(std::string(key));
    return it != object.end() and it->is_string() ? it->get<std::string>() : std::string{};
}

int intValue(const Json &object, std::string_view key)
{
    const auto it = object.find(std::string(key));
    return it != object.end() and it->is_number_integer() ? it->get<int>() : 0;
}

Json objectValue(const Json &object, std::string_view key)
{
    const auto it = object.find(std::string(key));
    return it != object.end() and it->is_object() ? *it : Json::object();
}

int hexDigit(char character)
{
    if (character >= '0' and character <= '9') return character - '0';
    if (character >= 'a' and character <= 'f') return character - 'a' + 10;
    if (character >= 'A' and character <= 'F') return character - 'A' + 10;
    return -1;
}

std::string percentDecoded(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' and index + 2 < value.size()) {
            const int high = hexDigit(value[index + 1]);
            const int low = hexDigit(value[index + 2]);
            if (high >= 0 and low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    return result;
}

std::string localPathForUri(std::string_view uri)
{
    constexpr std::string_view prefix = "file://";
    if (not uri.starts_with(prefix)) return {};
    std::string path = percentDecoded(uri.substr(prefix.size()));
    if (path.starts_with("localhost/")) path.erase(0, 9);
#if defined(_WIN32)
    if (path.size() >= 3 and path[0] == '/' and path[2] == ':') path.erase(0, 1);
#endif
    try {
        const auto *first = reinterpret_cast<const char8_t *>(path.data());
        const std::filesystem::path native = std::filesystem::absolute(
            std::filesystem::path(std::u8string(first, first + path.size())));
        const std::u8string encoded = native.u8string();
        return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
    } catch (const std::filesystem::filesystem_error &) {
        return {};
    }
}
}

BaaLanguageServer::BaaLanguageServer()
{
    m_compiler.setAnalysisCallback(
        [this](BaaAnalysisResult result) { onAnalysisFinished(std::move(result)); });
}

BaaLanguageServer::~BaaLanguageServer()
{
    m_compiler.setAnalysisCallback({});
    m_compiler.cancelAll();
}

void BaaLanguageServer::setCompilerProgram(std::string program)
{
    m_compiler.setCompilerProgram(std::move(program));
}

void BaaLanguageServer::setApplicationDirectory(std::filesystem::path directory)
{
    m_compiler.setApplicationDirectory(std::move(directory));
}

void BaaLanguageServer::setAnalysisDebounceInterval(int milliseconds)
{
    m_compiler.setDebounceInterval(milliseconds);
}

void BaaLanguageServer::setMessageCallback(MessageCallback callback)
{
    std::scoped_lock lock(m_callbackMutex);
    m_messageCallback = std::move(callback);
}

void BaaLanguageServer::setExitCallback(ExitCallback callback)
{
    std::scoped_lock lock(m_callbackMutex);
    m_exitCallback = std::move(callback);
}

void BaaLanguageServer::receiveMessage(std::string_view jsonBody)
{
    const Json message = Json::parse(jsonBody, nullptr, false);
    if (message.is_discarded() or not message.is_object()) {
        sendError(nullptr, ParseError, "Invalid JSON message.");
        return;
    }
    if (stringValue(message, "jsonrpc") != "2.0" or
        not message.contains("method") or not message["method"].is_string()) {
        sendError(message.value("id", Json(nullptr)), InvalidRequest,
                  "Invalid JSON-RPC request.");
        return;
    }
    if (message.contains("id")) handleRequest(message);
    else handleNotification(message);
}

void BaaLanguageServer::handleRequest(const Json &message)
{
    const Json id = message.value("id", Json(nullptr));
    const std::string method = stringValue(message, "method");
    if (method == "initialize") {
        if (m_initializeResponded) {
            sendError(id, InvalidRequest, "Language server is already initialized.");
            return;
        }
        m_initializeResponded = true;
        sendResult(id, {
            {"capabilities", {
                {"positionEncoding", "utf-16"},
                {"textDocumentSync", {
                    {"openClose", true},
                    {"change", 1},
                    {"save", {{"includeText", false}}}
                }}
            }},
            {"serverInfo", {{"name", "Baa-LSP"}, {"version", "0.1.0"}}}
        });
        return;
    }
    if (not m_initializeResponded) {
        sendError(id, ServerNotInitialized, "Language server is not initialized.");
        return;
    }
    if (method == "shutdown") {
        m_shutdownRequested = true;
        m_compiler.cancelAll();
        sendResult(id, nullptr);
        return;
    }
    sendError(id, MethodNotFound, "Method is not supported by this server version.");
}

void BaaLanguageServer::handleNotification(const Json &message)
{
    const std::string method = stringValue(message, "method");
    const Json params = objectValue(message, "params");
    if (method == "exit") {
        ExitCallback callback;
        {
            std::scoped_lock lock(m_callbackMutex);
            callback = m_exitCallback;
        }
        if (callback) callback(m_shutdownRequested ? 0 : 1);
        return;
    }
    if (method == "initialized") {
        if (m_initializeResponded and not m_shutdownRequested) m_initialized = true;
        return;
    }
    if (not m_initialized or m_shutdownRequested) return;

    if (method == "textDocument/didOpen") handleDidOpen(params);
    else if (method == "textDocument/didChange") handleDidChange(params);
    else if (method == "textDocument/didSave") handleDidSave(params);
    else if (method == "textDocument/didClose") handleDidClose(params);
}

void BaaLanguageServer::handleDidOpen(const Json &params)
{
    const Json item = objectValue(params, "textDocument");
    BaaDocument document{stringValue(item, "uri"), stringValue(item, "languageId"),
                         stringValue(item, "text"), intValue(item, "version")};
    std::string error;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.open(document, &error)) {
            sendLogMessage(error);
            return;
        }
    }
    analyze(document);
}

void BaaLanguageServer::handleDidChange(const Json &params)
{
    const Json item = objectValue(params, "textDocument");
    const std::string uri = stringValue(item, "uri");
    const int version = intValue(item, "version");
    const auto changes = params.find("contentChanges");
    if (changes == params.end() or not changes->is_array() or changes->empty() or
        not changes->front().is_object() or not changes->front().contains("text") or
        not changes->front()["text"].is_string()) {
        sendLogMessage("Document change does not contain valid full text.");
        return;
    }

    std::string error;
    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.change(uri, version, changes->front()["text"].get<std::string>(),
                                   &error)) {
            sendLogMessage(error, 2);
            return;
        }
        document = m_documents.document(uri);
    }
    analyze(document);
}

void BaaLanguageServer::handleDidSave(const Json &params)
{
    const std::string uri = stringValue(objectValue(params, "textDocument"), "uri");
    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(uri)) return;
        document = m_documents.document(uri);
    }
    analyze(document);
}

void BaaLanguageServer::handleDidClose(const Json &params)
{
    const std::string uri = stringValue(objectValue(params, "textDocument"), "uri");
    if (uri.empty()) return;
    m_compiler.cancel(uri);
    {
        std::scoped_lock lock(m_documentsMutex);
        m_documents.close(uri);
    }
    publishDiagnostics(uri, 0, Json::array());
}

void BaaLanguageServer::analyze(const BaaDocument &document)
{
    const std::string path = localPathForUri(document.uri);
    if (path.empty()) {
        sendLogMessage("This Baa-LSP version supports local file:// documents only.");
        return;
    }
    m_compiler.schedule({document.uri, path, document.text, document.version});
}

void BaaLanguageServer::onAnalysisFinished(BaaAnalysisResult result)
{
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(result.uri) or
            m_documents.document(result.uri).version != result.version) return;
    }
    if (not result.errorMessage.empty()) {
        sendLogMessage(result.errorMessage);
        return;
    }
    publishDiagnostics(result.uri, result.version,
                       PositionEncoding::baaDiagnosticsToLsp(result.text, result.diagnostics));
}

void BaaLanguageServer::publishDiagnostics(const std::string &uri,
                                           int version,
                                           const Json &diagnostics)
{
    Json params{{"uri", uri}, {"diagnostics", diagnostics}};
    if (version > 0) params["version"] = version;
    sendJson({{"jsonrpc", "2.0"},
              {"method", "textDocument/publishDiagnostics"},
              {"params", std::move(params)}});
}

void BaaLanguageServer::sendLogMessage(const std::string &message, int type)
{
    sendJson({{"jsonrpc", "2.0"}, {"method", "window/logMessage"},
              {"params", {{"type", type}, {"message", message}}}});
}

void BaaLanguageServer::sendResult(const Json &id, const Json &result)
{
    sendJson({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void BaaLanguageServer::sendError(const Json &id, int code, const std::string &message)
{
    sendJson({{"jsonrpc", "2.0"}, {"id", id},
              {"error", {{"code", code}, {"message", message}}}});
}

void BaaLanguageServer::sendJson(Json message)
{
    MessageCallback callback;
    {
        std::scoped_lock lock(m_callbackMutex);
        callback = m_messageCallback;
    }
    if (callback) callback(message.dump());
}
