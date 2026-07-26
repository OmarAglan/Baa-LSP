#include "server/BaaLanguageServer.h"

#include "lsp/PositionEncoding.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

namespace {
constexpr int ParseError = -32700;
constexpr int InvalidRequest = -32600;
constexpr int MethodNotFound = -32601;
constexpr int InvalidParams = -32602;
constexpr int InternalError = -32603;
constexpr int ServerNotInitialized = -32002;
constexpr int RequestCancelled = -32800;
constexpr int ContentModified = -32801;

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

std::filesystem::path pathFromUtf8(std::string_view path)
{
    const auto *first = reinterpret_cast<const char8_t *>(path.data());
    return std::filesystem::path(std::u8string(first, first + path.size()));
}

std::string utf8FromPath(const std::filesystem::path &path)
{
    const std::u8string encoded = path.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

std::string comparablePath(std::filesystem::path path)
{
    try {
        path = std::filesystem::absolute(path).lexically_normal();
    } catch (const std::filesystem::filesystem_error &) {
        return {};
    }
    std::string value = utf8FromPath(path);
#if defined(_WIN32)
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(
            character >= 'A' and character <= 'Z'
                ? character - 'A' + 'a' : character);
    });
#endif
    return value;
}

std::string percentEncodedPath(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        const bool asciiAlphaNumeric =
            (character >= 'A' and character <= 'Z') or
            (character >= 'a' and character <= 'z') or
            (character >= '0' and character <= '9');
        if (asciiAlphaNumeric or character == '-' or character == '_' or
            character == '.' or character == '~' or character == '/' or
            character == ':') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4u]);
            result.push_back(hex[character & 0x0Fu]);
        }
    }
    return result;
}

std::string fileUriForPath(const std::filesystem::path &input)
{
    try {
        std::filesystem::path path =
            std::filesystem::absolute(input).lexically_normal();
        const std::u8string encoded = path.generic_u8string();
        std::string generic{
            reinterpret_cast<const char *>(encoded.data()), encoded.size()};
#if defined(_WIN32)
        if (generic.empty() or generic.front() != '/') generic.insert(0, "/");
#endif
        return "file://" + percentEncodedPath(generic);
    } catch (const std::filesystem::filesystem_error &) {
        return {};
    }
}

std::optional<std::string> readUtf8File(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (not input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

Json semanticLocationToLsp(const std::string &rootUri,
                           const std::string &rootText,
                           const Json &location,
                           const std::unordered_map<std::string, std::string>
                               *openTexts = nullptr)
{
    if (not location.is_object()) return nullptr;
    const std::string rootPathText = localPathForUri(rootUri);
    const std::string locationPathText = location.value("file", "");
    if (rootPathText.empty() or locationPathText.empty()) return nullptr;

    std::filesystem::path rootPath = pathFromUtf8(rootPathText);
    std::filesystem::path locationPath = pathFromUtf8(locationPathText);
    if (locationPath.is_relative()) {
        locationPath = rootPath.parent_path() / locationPath;
    }
    if (comparablePath(rootPath) == comparablePath(locationPath)) {
        return PositionEncoding::baaLocationToLsp(rootText, rootUri, location);
    }

    const std::string uri = fileUriForPath(locationPath);
    if (uri.empty()) return nullptr;
    if (openTexts) {
        const auto open = openTexts->find(uri);
        if (open != openTexts->end())
            return PositionEncoding::baaLocationToLsp(
                open->second, uri, location);
    }
    const std::optional<std::string> text = readUtf8File(locationPath);
    if (not text) return nullptr;
    return PositionEncoding::baaLocationToLsp(*text, uri, location);
}

std::pair<std::uint32_t, std::size_t> completionCodePoint(std::string_view text,
                                                          std::size_t offset)
{
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80u) return {first, 1};
    std::size_t count = 1;
    std::uint32_t codePoint = 0xFFFDu;
    if ((first & 0xE0u) == 0xC0u) {
        count = 2;
        codePoint = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {
        count = 3;
        codePoint = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {
        count = 4;
        codePoint = first & 0x07u;
    }
    if (offset + count > text.size()) return {0xFFFDu, 1};
    for (std::size_t index = 1; index < count; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xC0u) != 0x80u) return {0xFFFDu, 1};
        codePoint = (codePoint << 6u) | (next & 0x3Fu);
    }
    return {codePoint, count};
}

std::size_t previousCodePointStart(std::string_view text, std::size_t offset)
{
    if (offset == 0) return 0;
    --offset;
    while (offset > 0 and
           (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u) --offset;
    return offset;
}

bool isBaaCompletionCodePoint(std::uint32_t codePoint)
{
    return codePoint == '#' or codePoint == '_' or
           (codePoint >= 0x0600u and codePoint <= 0x06FFu) or
           (codePoint >= 0x0750u and codePoint <= 0x077Fu) or
           (codePoint >= 0x08A0u and codePoint <= 0x08FFu) or
           (codePoint >= 0xFB50u and codePoint <= 0xFDFFu) or
           (codePoint >= 0xFE70u and codePoint <= 0xFEFFu);
}

bool isArabicLetter(std::uint32_t codePoint)
{
    return (codePoint >= 0x0621u and codePoint <= 0x063Au) or
           (codePoint >= 0x0641u and codePoint <= 0x064Au) or
           (codePoint >= 0x066Eu and codePoint <= 0x066Fu) or
           (codePoint >= 0x0671u and codePoint <= 0x06D3u) or
           codePoint == 0x06D5u or
           (codePoint >= 0x06EEu and codePoint <= 0x06EFu) or
           (codePoint >= 0x06FAu and codePoint <= 0x06FCu) or
           codePoint == 0x06FFu or
           (codePoint >= 0x0750u and codePoint <= 0x077Fu) or
           (codePoint >= 0x08A0u and codePoint <= 0x08C9u);
}

bool isArabicDigit(std::uint32_t codePoint)
{
    return (codePoint >= 0x0660u and codePoint <= 0x0669u) or
           (codePoint >= 0x06F0u and codePoint <= 0x06F9u);
}

bool isArabicIdentifier(std::string_view text)
{
    if (text.empty()) return false;
    bool first = true;
    bool hasLetter = false;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto [codePoint, bytes] = completionCodePoint(text, offset);
        if (codePoint == 0xFFFDu or bytes == 0) return false;
        if (first) {
            if (codePoint != '_' and not isArabicLetter(codePoint))
                return false;
        } else if (codePoint != '_' and not isArabicLetter(codePoint) and
                   not isArabicDigit(codePoint)) {
            return false;
        }
        hasLetter = hasLetter or isArabicLetter(codePoint);
        first = false;
        offset += bytes;
    }
    return hasLetter;
}

std::size_t completionPrefixStart(std::string_view text, std::size_t cursor)
{
    std::size_t start = std::min(cursor, text.size());
    while (start > 0) {
        const std::size_t previous = previousCodePointStart(text, start);
        const auto [codePoint, ignored] = completionCodePoint(text, previous);
        (void)ignored;
        if (not isBaaCompletionCodePoint(codePoint)) break;
        start = previous;
    }
    return start;
}

Json completionTriggerCharacters()
{
    return Json::array({
        "#", "_", "ا", "أ", "إ", "آ", "ب", "ت", "ث", "ج", "ح", "خ",
        "د", "ذ", "ر", "ز", "س", "ش", "ص", "ض", "ط", "ظ", "ع", "غ",
        "ف", "ق", "ك", "ل", "م", "ن", "ه", "و", "ي", "ى", "ة", "ؤ",
        "ئ", "ء"
    });
}

int metadataCompletionKind(std::string_view kind)
{
    if (kind == "snippet") return 15;
    if (kind == "value") return 12;
    if (kind == "type") return 7;
    return 14;
}

int symbolCompletionKind(std::string_view kind)
{
    if (kind == "function") return 3;
    if (kind == "field") return 5;
    if (kind == "variable" or kind == "parameter" or kind == "array") return 6;
    if (kind == "enum") return 13;
    if (kind == "enum-member") return 20;
    if (kind == "struct" or kind == "union") return 22;
    if (kind == "type-alias") return 25;
    return 6;
}

std::string completionTypeDisplay(const Json &symbol, std::string_view field)
{
    const auto it = symbol.find(std::string(field));
    return it != symbol.end() and it->is_object() ? it->value("display", "") : "";
}

std::string completionSymbolDetail(const Json &symbol)
{
    const std::string kind = symbol.value("kind", "");
    if (kind == "function") {
        const std::string type = completionTypeDisplay(symbol, "return_type");
        return type.empty() ? "دالة" : "دالة ← " + type;
    }
    if (kind == "array") {
        const std::string type = completionTypeDisplay(symbol, "element_type");
        return type.empty() ? "مصفوفة" : "مصفوفة من " + type;
    }
    if (kind == "type-alias") {
        const std::string type = completionTypeDisplay(symbol, "target_type");
        return type.empty() ? "اسم نوع" : "اسم نوع ← " + type;
    }
    return completionTypeDisplay(symbol, "type");
}

bool completionMatches(std::string_view candidate, std::string_view prefix)
{
    return prefix.empty() or candidate.starts_with(prefix);
}

Json buildCompletionItems(std::string_view text,
                          std::size_t prefixStart,
                          std::size_t cursor,
                          const Json &metadata,
                          const Json &symbols)
{
    const std::string prefix(text.substr(prefixStart, cursor - prefixStart));
    const bool directiveContext = prefix.starts_with('#');
    const Json replacementRange{
        {"start", PositionEncoding::utf16PositionForByteOffset(text, prefixStart)},
        {"end", PositionEncoding::utf16PositionForByteOffset(text, cursor)}
    };
    Json result = Json::array();
    std::unordered_set<std::string> seen;

    if (metadata.is_array()) {
        for (const Json &source : metadata) {
            if (not source.is_object()) continue;
            const std::string label = source.value("label", "");
            const std::string filter = source.value("filter_text", label);
            const std::string kind = source.value("kind", "keyword");
            const bool isDirective = kind == "directive";
            if (label.empty() or filter.empty() or isDirective != directiveContext or
                not completionMatches(filter, prefix)) continue;
            const std::string insertion = source.value("insert_text", filter);
            const std::string key = kind + "\n" + label + "\n" + insertion;
            if (not seen.insert(key).second) continue;

            Json item{
                {"label", label},
                {"kind", metadataCompletionKind(kind)},
                {"filterText", filter},
                {"insertTextFormat",
                 source.value("insert_text_format", "plain") == "snippet" ? 2 : 1},
                {"sortText", (kind == "snippet" ? "2" : "1") + filter},
                {"textEdit", {{"range", replacementRange}, {"newText", insertion}}}
            };
            const std::string detail = source.value("detail", "");
            if (not detail.empty()) item["detail"] = detail;
            result.push_back(std::move(item));
        }
    }

    if (not directiveContext and symbols.is_array()) {
        for (const Json &symbol : symbols) {
            if (not symbol.is_object()) continue;
            const std::string name = symbol.value("name", "");
            const std::string kind = symbol.value("kind", "");
            if (name.empty() or not completionMatches(name, prefix)) continue;
            const std::string key = "symbol\n" + name;
            if (not seen.insert(key).second) continue;
            Json item{
                {"label", name},
                {"kind", symbolCompletionKind(kind)},
                {"filterText", name},
                {"insertTextFormat", 1},
                {"sortText", "0" + name},
                {"textEdit", {{"range", replacementRange}, {"newText", name}}}
            };
            const std::string detail = completionSymbolDetail(symbol);
            if (not detail.empty()) item["detail"] = detail;
            result.push_back(std::move(item));
        }
    }
    return result;
}
}

BaaLanguageServer::BaaLanguageServer()
{
    m_compiler.setAnalysisCallback(
        [this](BaaAnalysisResult result) { onAnalysisFinished(std::move(result)); });
    m_compiler.setSymbolCallback(
        [this](BaaSymbolResult result) { onSymbolsFinished(std::move(result)); });
    m_compiler.setCompletionDataCallback(
        [this](BaaCompletionDataResult result) {
            onCompletionDataFinished(std::move(result));
        });
    m_compiler.setSemanticCallback(
        [this](BaaSemanticResult result) { onSemanticFinished(std::move(result)); });
}

BaaLanguageServer::~BaaLanguageServer()
{
    m_compiler.setAnalysisCallback({});
    m_compiler.setSymbolCallback({});
    m_compiler.setCompletionDataCallback({});
    m_compiler.setSemanticCallback({});
    m_compiler.cancelAll();
}

void BaaLanguageServer::setCompilerProgram(std::string program)
{
    m_compiler.setCompilerProgram(std::move(program));
}

void BaaLanguageServer::setTakweenProgram(std::string program)
{
    m_takweenProgram = std::move(program);
}

void BaaLanguageServer::setApplicationDirectory(std::filesystem::path directory)
{
    m_applicationDirectory = directory;
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

void BaaLanguageServer::loadProjectPlan(const Json &initializeParams)
{
    m_projectPlan = {};
    std::string rootUri = stringValue(initializeParams, "rootUri");
    if (rootUri.empty()) {
        const auto folders = initializeParams.find("workspaceFolders");
        if (folders != initializeParams.end() and folders->is_array() and
            not folders->empty() and folders->front().is_object()) {
            rootUri = stringValue(folders->front(), "uri");
        }
    }
    const std::string rootPathText = localPathForUri(rootUri);
    if (rootPathText.empty()) return;

    const std::filesystem::path root = pathFromUtf8(rootPathText);
    const std::filesystem::path manifest =
        root / pathFromUtf8("مشروع.تكوين");
    std::error_code filesystemError;
    if (not std::filesystem::is_regular_file(manifest, filesystemError)) return;

    const Json options = objectValue(initializeParams, "initializationOptions");
    std::string program = stringValue(options, "takweenPath");
    if (program.empty()) program = m_takweenProgram;
    if (program.empty()) {
        if (const char *environment = std::getenv("TAKWEEN"))
            program = environment;
    }
    if (program.empty() and not m_applicationDirectory.empty()) {
#if defined(_WIN32)
        const std::vector<std::filesystem::path> candidates{
            m_applicationDirectory / pathFromUtf8("takween/تكوين.exe"),
            m_applicationDirectory / "takween/takween.exe",
            m_applicationDirectory / pathFromUtf8("تكوين.exe"),
            m_applicationDirectory / "takween.exe"
        };
#else
        const std::vector<std::filesystem::path> candidates{
            m_applicationDirectory / pathFromUtf8("takween/تكوين"),
            m_applicationDirectory / "takween/takween",
            m_applicationDirectory / pathFromUtf8("تكوين"),
            m_applicationDirectory / "takween"
        };
#endif
        for (const std::filesystem::path &candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate, filesystemError)) {
                program = utf8FromPath(candidate);
                break;
            }
        }
    }
#if defined(_WIN32)
    if (program.empty()) program = "تكوين.exe";
#else
    if (program.empty()) program = "تكوين";
#endif

    std::vector<std::string> arguments{"خطة", "--جسون"};
    const std::string target = stringValue(options, "takweenTarget");
    if (not target.empty()) arguments.push_back(target);

    m_projectRunner.prepare();
    ProcessResult process = m_projectRunner.run(program, arguments, root, {});
    if (not process.started and
        stringValue(options, "takweenPath").empty() and
        m_takweenProgram.empty()) {
#if defined(_WIN32)
        program = "takween.exe";
#else
        program = "takween";
#endif
        m_projectRunner.prepare();
        process = m_projectRunner.run(program, arguments, root, {});
    }
    if (not process.started) {
        sendLogMessage(
            "Takween project plan is unavailable: " +
            (process.errorMessage.empty()
                ? std::string("the executable was not found.")
                : process.errorMessage),
            2);
        return;
    }
    if (process.exitCode != 0) {
        std::string message =
            "Takween project plan failed with exit code " +
            std::to_string(process.exitCode) + ".";
        if (not process.standardError.empty())
            message += " " + process.standardError;
        sendLogMessage(message, 2);
        return;
    }

    const Json plan = Json::parse(process.standardOutput, nullptr, false);
    if (plan.is_discarded() or not plan.is_object() or
        plan.value("schema_version", "") != "takween-build-plan-v1" or
        not plan.value("source_files", Json::array()).is_array() or
        not plan.value("include_paths", Json::array()).is_array()) {
        sendLogMessage(
            "Takween returned a project plan that does not satisfy "
            "takween-build-plan-v1.", 2);
        return;
    }

    std::filesystem::path workingDirectory = pathFromUtf8(
        plan.value("working_directory", std::string(".")));
    if (workingDirectory.is_relative()) workingDirectory = root / workingDirectory;
    workingDirectory = std::filesystem::absolute(
        workingDirectory).lexically_normal();

    ProjectPlan loaded;
    loaded.root = std::filesystem::absolute(root).lexically_normal();
    loaded.workingDirectory = workingDirectory;
    std::unordered_set<std::string> seenSources;
    for (const Json &source : plan["source_files"]) {
        if (not source.is_string()) continue;
        std::filesystem::path path = pathFromUtf8(source.get<std::string>());
        if (path.is_relative()) path = workingDirectory / path;
        path = std::filesystem::absolute(path).lexically_normal();
        if (path.extension() != ".baa") continue;
        const std::string comparable = comparablePath(path);
        if (not comparable.empty() and seenSources.insert(comparable).second)
            loaded.sourceFiles.push_back(std::move(path));
    }
    for (const Json &include : plan["include_paths"]) {
        if (not include.is_string()) continue;
        std::filesystem::path path = pathFromUtf8(include.get<std::string>());
        if (path.is_relative()) path = workingDirectory / path;
        loaded.includePaths.push_back(
            utf8FromPath(std::filesystem::absolute(path).lexically_normal()));
    }
    loaded.loaded = not loaded.sourceFiles.empty();
    m_projectPlan = std::move(loaded);
    if (m_projectPlan.loaded) {
        sendLogMessage(
            "Loaded Takween project plan with " +
            std::to_string(m_projectPlan.sourceFiles.size()) +
            " Baa translation units.", 3);
    }
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
        const Json initializeParams = objectValue(message, "params");
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
                }},
                {"documentSymbolProvider", true},
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"referencesProvider", true},
                {"renameProvider", {{"prepareProvider", true}}},
                {"signatureHelpProvider", {
                    {"triggerCharacters", Json::array({"(", "،", ","})},
                    {"retriggerCharacters", Json::array({"،", ","})}
                }},
                {"completionProvider", {
                    {"resolveProvider", false},
                    {"triggerCharacters", completionTriggerCharacters()}
                }}
            }},
            {"serverInfo", {{"name", "Baa-LSP"}, {"version", "0.1.0"}}}
        });
        {
            std::scoped_lock lock(m_completionMutex);
            m_completionDataState = CompletionDataState::Loading;
            m_completionDataError.clear();
        }
        m_compiler.requestCompletionData();
        loadProjectPlan(initializeParams);
        return;
    }
    if (not m_initializeResponded) {
        sendError(id, ServerNotInitialized, "Language server is not initialized.");
        return;
    }
    if (method == "shutdown") {
        m_shutdownRequested = true;
        invalidateSymbolRequests({}, RequestCancelled, "Request cancelled during shutdown.");
        invalidateCompletionRequests({}, RequestCancelled,
                                     "Request cancelled during shutdown.");
        invalidateSemanticRequests({}, RequestCancelled,
                                   "Request cancelled during shutdown.");
        m_compiler.cancelAll();
        sendResult(id, nullptr);
        return;
    }
    if (m_shutdownRequested) {
        sendError(id, InvalidRequest, "Language server is shutting down.");
        return;
    }
    if (not m_initialized) {
        sendError(id, ServerNotInitialized,
                  "Language server has not received the initialized notification.");
        return;
    }
    if (method == "textDocument/documentSymbol") {
        handleDocumentSymbol(id, objectValue(message, "params"));
        return;
    }
    if (method == "textDocument/completion") {
        handleCompletion(id, objectValue(message, "params"));
        return;
    }
    if (method == "textDocument/hover") {
        handleSemanticRequest(
            id, objectValue(message, "params"), SemanticReplyKind::Hover);
        return;
    }
    if (method == "textDocument/signatureHelp") {
        handleSemanticRequest(
            id, objectValue(message, "params"), SemanticReplyKind::SignatureHelp);
        return;
    }
    if (method == "textDocument/definition") {
        handleSemanticRequest(
            id, objectValue(message, "params"), SemanticReplyKind::Definition);
        return;
    }
    if (method == "textDocument/references") {
        handleSemanticRequest(
            id, objectValue(message, "params"), SemanticReplyKind::References);
        return;
    }
    if (method == "textDocument/prepareRename") {
        handleSemanticRequest(
            id, objectValue(message, "params"), SemanticReplyKind::PrepareRename);
        return;
    }
    if (method == "textDocument/rename") {
        handleSemanticRequest(
            id, objectValue(message, "params"), SemanticReplyKind::Rename);
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
    if (method == "$/cancelRequest") {
        handleCancelRequest(params);
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
    invalidateSymbolRequests(uri, ContentModified,
                             "Document changed before symbols were ready.");
    invalidateCompletionRequests(uri, ContentModified,
                                 "Document changed before completion was ready.");
    invalidateSemanticRequests(uri, ContentModified,
                               "Document changed before semantic data was ready.");
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        m_symbolCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_semanticMutex);
        if (m_projectPlan.loaded) m_semanticCache.clear();
        else m_semanticCache.erase(uri);
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
    invalidateSymbolRequests(uri, ContentModified,
                             "Document closed before symbols were ready.");
    invalidateCompletionRequests(uri, ContentModified,
                                 "Document closed before completion was ready.");
    invalidateSemanticRequests(uri, ContentModified,
                               "Document closed before semantic data was ready.");
    m_compiler.cancel(uri);
    {
        std::scoped_lock lock(m_documentsMutex);
        m_documents.close(uri);
    }
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        m_symbolCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_semanticMutex);
        if (m_projectPlan.loaded) m_semanticCache.clear();
        else m_semanticCache.erase(uri);
    }
    publishDiagnostics(uri, 0, Json::array());
}

void BaaLanguageServer::handleDocumentSymbol(const Json &id, const Json &params)
{
    const std::string uri = stringValue(objectValue(params, "textDocument"), "uri");
    if (uri.empty()) {
        sendError(id, InvalidParams, "textDocument.uri is required.");
        return;
    }

    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(uri)) {
            sendError(id, InvalidParams, "Document is not open in Baa-LSP.");
            return;
        }
        document = m_documents.document(uri);
    }
    Json cached;
    std::string cachedText;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        const auto it = m_symbolCache.find(uri);
        if (it != m_symbolCache.end() and it->second.version == document.version) {
            cached = it->second.symbols;
            cachedText = it->second.text;
        }
    }
    if (cached.is_array()) {
        sendResult(id, PositionEncoding::baaSymbolsToLsp(cachedText, cached));
        return;
    }
    requestSymbolsForDocument(document, &id);
}

void BaaLanguageServer::requestSymbolsForDocument(const BaaDocument &document,
                                                  const Json *requestId)
{
    const std::string path = localPathForUri(document.uri);
    if (path.empty()) {
        if (requestId) {
            sendError(*requestId, InvalidParams,
                      "Baa-LSP supports local file:// documents only.");
        }
        return;
    }

    std::uint64_t token{};
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        for (auto &[existingToken, pending] : m_symbolRequests) {
            if (pending.uri == document.uri and pending.version == document.version) {
                if (requestId) pending.ids.push_back(*requestId);
                return;
            }
        }
        token = m_nextSymbolToken++;
        if (token == 0) token = m_nextSymbolToken++;
        PendingSymbolRequest pending;
        pending.uri = document.uri;
        pending.version = document.version;
        if (requestId) pending.ids.push_back(*requestId);
        m_symbolRequests.emplace(token, std::move(pending));
    }
    m_compiler.requestSymbols(
        {token, document.uri, path, document.text, document.version});
}

void BaaLanguageServer::handleCompletion(const Json &id, const Json &params)
{
    const std::string uri = stringValue(objectValue(params, "textDocument"), "uri");
    const Json position = objectValue(params, "position");
    if (uri.empty() or not position.contains("line") or
        not position["line"].is_number_integer() or
        not position.contains("character") or
        not position["character"].is_number_integer()) {
        sendError(id, InvalidParams,
                  "textDocument.uri and a UTF-16 position are required.");
        return;
    }

    int version{};
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(uri)) {
            sendError(id, InvalidParams, "Document is not open in Baa-LSP.");
            return;
        }
        version = m_documents.document(uri).version;
    }

    const int line = position["line"].get<int>();
    const int character = position["character"].get<int>();
    if (line < 0 or character < 0) {
        sendError(id, InvalidParams, "Completion position cannot be negative.");
        return;
    }

    bool ready = false;
    bool startLoading = false;
    std::string failure;
    {
        std::scoped_lock lock(m_completionMutex);
        ready = m_completionDataState == CompletionDataState::Ready;
        if (m_completionDataState == CompletionDataState::Failed) {
            failure = m_completionDataError;
        } else if (not ready) {
            m_pendingCompletionRequests.push_back(
                {id, uri, version, line, character});
            if (m_completionDataState == CompletionDataState::NotRequested) {
                m_completionDataState = CompletionDataState::Loading;
                startLoading = true;
            }
        }
    }
    if (not failure.empty()) {
        sendError(id, InternalError, failure);
        return;
    }
    if (startLoading) m_compiler.requestCompletionData();
    if (ready) completeRequest(id, uri, version, line, character);
}

void BaaLanguageServer::completeRequest(const Json &id,
                                        const std::string &uri,
                                        int version,
                                        int line,
                                        int character)
{
    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(uri) or
            m_documents.document(uri).version != version) {
            sendError(id, ContentModified,
                      "Document changed before completion was ready.");
            return;
        }
        document = m_documents.document(uri);
    }

    Json metadata;
    {
        std::scoped_lock lock(m_completionMutex);
        metadata = m_completionItems;
    }
    Json symbols = Json::array();
    bool hasCurrentSymbols = false;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        const auto it = m_symbolCache.find(uri);
        if (it != m_symbolCache.end() and it->second.version == version) {
            symbols = it->second.symbols;
            hasCurrentSymbols = true;
        }
    }

    const std::size_t cursor = PositionEncoding::utf8ByteOffsetForUtf16Position(
        document.text, line, character);
    const std::size_t prefixStart = completionPrefixStart(document.text, cursor);
    sendResult(id, {
        {"isIncomplete", not hasCurrentSymbols},
        {"items", buildCompletionItems(document.text, prefixStart, cursor,
                                       metadata, symbols)}
    });
}

void BaaLanguageServer::handleSemanticRequest(const Json &id,
                                              const Json &params,
                                              SemanticReplyKind kind)
{
    const std::string uri = stringValue(objectValue(params, "textDocument"), "uri");
    const Json position = objectValue(params, "position");
    if (uri.empty() or !position.contains("line") or
        not position["line"].is_number_integer() or
        !position.contains("character") or
        not position["character"].is_number_integer()) {
        sendError(id, InvalidParams,
                  "textDocument.uri and a UTF-16 position are required.");
        return;
    }
    const int line = position["line"].get<int>();
    const int character = position["character"].get<int>();
    if (line < 0 or character < 0) {
        sendError(id, InvalidParams, "Semantic query position cannot be negative.");
        return;
    }

    BaaDocument document;
    std::vector<BaaDocument> openDocuments;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(uri)) {
            sendError(id, InvalidParams, "Document is not open in Baa-LSP.");
            return;
        }
        document = m_documents.document(uri);
        openDocuments = m_documents.documents();
    }
    const std::size_t positionByte =
        PositionEncoding::utf8ByteOffsetForUtf16Position(
            document.text, line, character);
    const bool includeDeclaration =
        kind == SemanticReplyKind::References and
        objectValue(params, "context").value("includeDeclaration", false);
    std::string newName;
    if (kind == SemanticReplyKind::Rename) {
        newName = stringValue(params, "newName");
        if (newName.empty()) {
            sendError(id, InvalidParams,
                      "textDocument/rename requires a non-empty newName.");
            return;
        }
    }
    PendingSemanticReply reply;
    reply.id = id;
    reply.kind = kind;
    reply.includeDeclaration = includeDeclaration;
    reply.newName = std::move(newName);

    {
        std::scoped_lock lock(m_semanticMutex);
        const auto cached = m_semanticCache.find(uri);
        if (cached != m_semanticCache.end() and
            cached->second.version == document.version and
            cached->second.positionByte == positionByte) {
            if (kind == SemanticReplyKind::Rename) {
                std::string error;
                Json edit = renameWorkspaceEdit(
                    reply, uri, cached->second, &error);
                if (not error.empty()) sendError(id, InvalidParams, error);
                else sendResult(id, edit);
            } else {
                sendResult(id, convertSemanticReply(reply, uri, cached->second));
            }
            return;
        }
    }

    const std::string path = localPathForUri(uri);
    if (path.empty()) {
        sendError(id, InvalidParams,
                  "Baa-LSP supports local file:// documents only.");
        return;
    }

    std::uint64_t token = 0;
    BaaSemanticRequest compilerRequest;
    {
        std::scoped_lock lock(m_semanticMutex);
        for (auto &[existingToken, pending] : m_semanticRequests) {
            if (pending.uri == uri and pending.version == document.version and
                pending.positionByte == positionByte) {
                pending.replies.push_back(reply);
                return;
            }
        }
        token = m_nextSemanticToken++;
        if (token == 0) token = m_nextSemanticToken++;
        PendingSemanticRequest pending;
        pending.uri = uri;
        pending.version = document.version;
        pending.positionByte = positionByte;
        pending.replies.push_back(reply);
        compilerRequest.token = token;
        compilerRequest.uri = uri;
        compilerRequest.filePath = path;
        compilerRequest.text = document.text;
        compilerRequest.version = document.version;
        compilerRequest.positionByte = positionByte;
        if (m_projectPlan.loaded) {
            compilerRequest.projectWorkingDirectory =
                m_projectPlan.workingDirectory;
            compilerRequest.includePaths = m_projectPlan.includePaths;

            std::unordered_map<std::string, BaaDocument> openByPath;
            for (const BaaDocument &open : openDocuments) {
                const std::string openPath = localPathForUri(open.uri);
                const std::string comparable =
                    comparablePath(pathFromUtf8(openPath));
                if (not comparable.empty())
                    openByPath.insert_or_assign(comparable, open);
            }
            bool originIncluded = false;
            for (const std::filesystem::path &source :
                 m_projectPlan.sourceFiles) {
                BaaSemanticRequest::ProjectSource projectSource;
                projectSource.filePath = utf8FromPath(source);
                const std::string comparable = comparablePath(source);
                const auto open = openByPath.find(comparable);
                if (open != openByPath.end()) {
                    projectSource.uri = open->second.uri;
                    projectSource.text = open->second.text;
                    projectSource.version = open->second.version;
                    projectSource.useStandardInput = true;
                    pending.projectTexts.insert_or_assign(
                        open->second.uri, open->second.text);
                    pending.projectVersions.insert_or_assign(
                        open->second.uri, open->second.version);
                } else {
                    projectSource.uri = fileUriForPath(source);
                }
                if (comparable == comparablePath(pathFromUtf8(path)))
                    originIncluded = true;
                compilerRequest.projectSources.push_back(
                    std::move(projectSource));
            }
            if (not originIncluded) {
                compilerRequest.projectSources.push_back({
                    uri, path, document.text, document.version, true
                });
                pending.projectTexts.insert_or_assign(uri, document.text);
                pending.projectVersions.insert_or_assign(
                    uri, document.version);
            }
        } else {
            compilerRequest.projectSources.push_back({
                uri, path, document.text, document.version, true
            });
            pending.projectTexts.insert_or_assign(uri, document.text);
            pending.projectVersions.insert_or_assign(uri, document.version);
        }
        m_semanticRequests.emplace(token, std::move(pending));
    }
    m_compiler.requestSemantic(std::move(compilerRequest));
}

void BaaLanguageServer::handleCancelRequest(const Json &params)
{
    const auto idIt = params.find("id");
    if (idIt == params.end()) return;

    bool cancelled = false;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        for (auto &[token, pending] : m_symbolRequests) {
            (void)token;
            const auto match = std::ranges::find(pending.ids, *idIt);
            if (match != pending.ids.end()) {
                pending.ids.erase(match);
                cancelled = true;
                break;
            }
        }
    }
    if (not cancelled) {
        std::scoped_lock lock(m_completionMutex);
        const auto match = std::ranges::find_if(
            m_pendingCompletionRequests,
            [&idIt](const PendingCompletionRequest &request) {
                return request.id == *idIt;
            });
        if (match != m_pendingCompletionRequests.end()) {
            m_pendingCompletionRequests.erase(match);
            cancelled = true;
        }
    }
    std::uint64_t semanticTokenToCancel = 0;
    if (not cancelled) {
        std::scoped_lock lock(m_semanticMutex);
        for (auto request = m_semanticRequests.begin();
             request != m_semanticRequests.end(); ++request) {
            const auto reply = std::ranges::find_if(
                request->second.replies,
                [&idIt](const PendingSemanticReply &candidate) {
                    return candidate.id == *idIt;
                });
            if (reply == request->second.replies.end()) continue;
            request->second.replies.erase(reply);
            cancelled = true;
            if (request->second.replies.empty()) {
                semanticTokenToCancel = request->first;
                m_semanticRequests.erase(request);
            }
            break;
        }
    }
    if (semanticTokenToCancel != 0)
        m_compiler.cancelSemantic(semanticTokenToCancel);
    if (cancelled) {
        sendError(*idIt, RequestCancelled, "Request cancelled by the client.");
    }
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
    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(result.uri) or
            m_documents.document(result.uri).version != result.version) return;
        document = m_documents.document(result.uri);
    }
    if (not result.errorMessage.empty()) {
        sendLogMessage(result.errorMessage);
        return;
    }
    publishDiagnostics(result.uri, result.version,
                       PositionEncoding::baaDiagnosticsToLsp(result.text, result.diagnostics));
    if (result.exitCode == 0) requestSymbolsForDocument(document, nullptr);
}

void BaaLanguageServer::onSymbolsFinished(BaaSymbolResult result)
{
    PendingSymbolRequest pending;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        const auto it = m_symbolRequests.find(result.token);
        if (it == m_symbolRequests.end()) return;
        pending = it->second;
        m_symbolRequests.erase(it);
    }

    bool current = false;
    {
        std::scoped_lock lock(m_documentsMutex);
        current = m_documents.contains(pending.uri) and
                  m_documents.document(pending.uri).version == pending.version;
    }
    if (not current) {
        for (const Json &id : pending.ids) {
            sendError(id, ContentModified,
                      "Document changed before symbols were ready.");
        }
        return;
    }
    if (not result.errorMessage.empty()) {
        for (const Json &id : pending.ids) {
            sendError(id, InternalError, result.errorMessage);
        }
        return;
    }
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        m_symbolCache.insert_or_assign(
            pending.uri,
            CachedSymbols{pending.version, result.text, result.symbols});
    }
    const Json converted = PositionEncoding::baaSymbolsToLsp(result.text, result.symbols);
    for (const Json &id : pending.ids) sendResult(id, converted);
}

void BaaLanguageServer::onCompletionDataFinished(BaaCompletionDataResult result)
{
    std::vector<PendingCompletionRequest> pending;
    std::string failure;
    {
        std::scoped_lock lock(m_completionMutex);
        if (result.errorMessage.empty()) {
            m_completionItems = std::move(result.items);
            m_completionDataState = CompletionDataState::Ready;
            m_completionDataError.clear();
        } else {
            m_completionItems = Json::array();
            m_completionDataState = CompletionDataState::Failed;
            m_completionDataError = std::move(result.errorMessage);
            failure = m_completionDataError;
        }
        pending.swap(m_pendingCompletionRequests);
    }
    if (not failure.empty()) {
        sendLogMessage(failure);
        for (const PendingCompletionRequest &request : pending) {
            sendError(request.id, InternalError, failure);
        }
        return;
    }
    for (const PendingCompletionRequest &request : pending) {
        completeRequest(request.id, request.uri, request.version,
                        request.line, request.character);
    }
}

Json BaaLanguageServer::convertSemanticReply(
    const PendingSemanticReply &reply,
    const std::string &uri,
    const CachedSemanticQuery &query)
{
    switch (reply.kind) {
        case SemanticReplyKind::Hover:
            return PositionEncoding::baaSemanticHoverToLsp(
                query.text, query.hover);
        case SemanticReplyKind::SignatureHelp:
            return PositionEncoding::baaSignatureHelpToLsp(
                query.signatureHelp);
        case SemanticReplyKind::Definition:
            if (query.projectOccurrences.is_array() and
                not query.projectOccurrences.empty()) {
                const Json *declaration = nullptr;
                for (const Json &occurrence : query.projectOccurrences) {
                    if (not occurrence.is_object() or
                        not occurrence.contains("location"))
                        continue;
                    const std::string role =
                        occurrence.value("role", "");
                    if (role == "definition") {
                        return semanticLocationToLsp(
                            uri, query.text, occurrence["location"],
                            &query.projectTexts);
                    }
                    if (role == "declaration" and not declaration)
                        declaration = &occurrence["location"];
                }
                if (declaration)
                    return semanticLocationToLsp(
                        uri, query.text, *declaration,
                        &query.projectTexts);
            }
            return semanticLocationToLsp(uri, query.text, query.definition);
        case SemanticReplyKind::References:
        {
            Json converted = Json::array();
            if (query.projectOccurrences.is_array() and
                not query.projectOccurrences.empty()) {
                std::unordered_set<std::string> seen;
                for (const Json &occurrence : query.projectOccurrences) {
                    if (not occurrence.is_object() or
                        not occurrence.contains("location"))
                        continue;
                    const std::string role =
                        occurrence.value("role", "");
                    if (not reply.includeDeclaration and
                        (role == "declaration" or role == "definition"))
                        continue;
                    Json location = semanticLocationToLsp(
                        uri, query.text, occurrence["location"],
                        &query.projectTexts);
                    if (location.is_null()) continue;
                    const std::string key = location.dump();
                    if (seen.insert(key).second)
                        converted.push_back(std::move(location));
                }
                return converted;
            }
            if (not query.references.is_array()) return converted;
            for (const Json &reference : query.references) {
                if (not reply.includeDeclaration and
                    reference.value("role", "") == "declaration")
                    continue;
                Json location = semanticLocationToLsp(uri, query.text, reference);
                if (not location.is_null()) {
                    converted.push_back(std::move(location));
                }
            }
            return converted;
        }
        case SemanticReplyKind::PrepareRename:
        {
            if (not query.symbol.is_object()) return nullptr;
            const std::string placeholder =
                query.symbol.value("name", "");
            if (placeholder.empty()) return nullptr;

            auto prepareFrom = [&](const Json &items,
                                   bool wrapped) -> Json {
                if (not items.is_array()) return nullptr;
                for (const Json &item : items) {
                    const Json location = wrapped
                        ? item.value("location", Json(nullptr)) : item;
                    if (not location.is_object()) continue;
                    const Json range = location.value(
                        "range", Json::object());
                    const Json start = range.value(
                        "start", Json::object());
                    const Json end = range.value(
                        "end", Json::object());
                    if (not start.contains("byte") or
                        not start["byte"].is_number_integer() or
                        not end.contains("byte") or
                        not end["byte"].is_number_integer())
                        continue;
                    const std::size_t startByte =
                        start["byte"].get<std::size_t>();
                    const std::size_t endByte =
                        end["byte"].get<std::size_t>();
                    if (query.positionByte < startByte or
                        query.positionByte > endByte)
                        continue;
                    Json converted = semanticLocationToLsp(
                        uri, query.text, location, &query.projectTexts);
                    if (converted.is_object() and
                        converted.contains("range")) {
                        return Json{
                            {"range", converted["range"]},
                            {"placeholder", placeholder}
                        };
                    }
                }
                return nullptr;
            };

            Json prepared = prepareFrom(query.projectOccurrences, true);
            if (not prepared.is_null()) return prepared;
            return prepareFrom(query.references, false);
        }
        case SemanticReplyKind::Rename:
            return nullptr;
    }
    return nullptr;
}

Json BaaLanguageServer::renameWorkspaceEdit(
    const PendingSemanticReply &reply,
    const std::string &uri,
    const CachedSemanticQuery &query,
    std::string *error)
{
    if (error) error->clear();
    auto reject = [error](std::string message) {
        if (error) *error = std::move(message);
        return Json(nullptr);
    };
    if (not query.symbol.is_object())
        return reject("لا يوجد رمز باء قابل لإعادة التسمية عند هذا الموضع.");
    if (not query.projectIndexComplete)
        return reject(
            "تعذر فحص كل ملفات المشروع؛ أُلغيت إعادة التسمية حفاظا على سلامة الشفرة.");
    if (not isArabicIdentifier(reply.newName))
        return reject(
            "يجب أن يكون الاسم الجديد معرف باء عربيا صالحا دون أحرف لاتينية.");

    const std::string oldName = query.symbol.value("name", "");
    if (oldName.empty())
        return reject("لم يُرجع المصرّف اسم الرمز المحدد.");
    if (reply.newName == oldName)
        return reject("الاسم الجديد مطابق للاسم الحالي.");

    {
        std::scoped_lock lock(m_completionMutex);
        for (const Json &item : m_completionItems) {
            if (not item.is_object() or
                item.value("label", "") != reply.newName)
                continue;
            const std::string kind = item.value("kind", "");
            if (kind == "keyword" or kind == "type" or kind == "value")
                return reject(
                    "الاسم الجديد كلمة محجوزة في باء ولا يصلح معرفا.");
        }
    }

    if (not query.projectIndexOccurrences.is_array() or
        query.projectIndexOccurrences.empty())
        return reject(
            "لم يكتمل فهرس الرموز المملوك للمصرّف؛ أُلغيت إعادة التسمية.");

    for (const Json &occurrence : query.projectIndexOccurrences) {
        if (not occurrence.is_object()) continue;
        const std::string role = occurrence.value("role", "");
        if (role != "declaration" and role != "definition") continue;
        const Json candidate = occurrence.value("symbol", Json(nullptr));
        if (not candidate.is_object() or candidate == query.symbol or
            candidate.value("name", "") != reply.newName)
            continue;
        return reject(
            "يتعارض الاسم الجديد مع رمز آخر حدده مصرّف باء في المشروع.");
    }

    std::map<std::string, std::vector<Json>> editsByUri;
    std::unordered_set<std::string> seen;
    auto addLocation = [&](const Json &location) {
        Json converted = semanticLocationToLsp(
            uri, query.text, location, &query.projectTexts);
        if (not converted.is_object() or
            not converted.contains("uri") or
            not converted["uri"].is_string() or
            not converted.contains("range"))
            return;
        const std::string editUri = converted["uri"].get<std::string>();
        const std::string key =
            editUri + "\n" + converted["range"].dump();
        if (not seen.insert(key).second) return;
        editsByUri[editUri].push_back({
            {"range", converted["range"]},
            {"newText", reply.newName}
        });
    };

    if (query.projectOccurrences.is_array()) {
        for (const Json &occurrence : query.projectOccurrences) {
            if (occurrence.is_object() and
                occurrence.contains("location"))
                addLocation(occurrence["location"]);
        }
    }
    if (editsByUri.empty() and query.references.is_array()) {
        for (const Json &location : query.references)
            addLocation(location);
    }
    if (editsByUri.empty())
        return reject("لم يعثر المصرّف على مواضع آمنة لإعادة التسمية.");

    Json documentChanges = Json::array();
    for (auto &[editUri, edits] : editsByUri) {
        std::ranges::sort(edits, [](const Json &left, const Json &right) {
            const Json leftStart =
                left["range"].value("start", Json::object());
            const Json rightStart =
                right["range"].value("start", Json::object());
            return std::pair{
                leftStart.value("line", 0),
                leftStart.value("character", 0)
            } < std::pair{
                rightStart.value("line", 0),
                rightStart.value("character", 0)
            };
        });
        Json textDocument{{"uri", editUri}, {"version", nullptr}};
        const auto version = query.projectVersions.find(editUri);
        if (version != query.projectVersions.end())
            textDocument["version"] = version->second;
        documentChanges.push_back({
            {"textDocument", std::move(textDocument)},
            {"edits", edits}
        });
    }
    return Json{{"documentChanges", std::move(documentChanges)}};
}

void BaaLanguageServer::onSemanticFinished(BaaSemanticResult result)
{
    PendingSemanticRequest pending;
    {
        std::scoped_lock lock(m_semanticMutex);
        const auto request = m_semanticRequests.find(result.token);
        if (request == m_semanticRequests.end()) return;
        pending = std::move(request->second);
        m_semanticRequests.erase(request);
    }

    bool current = false;
    {
        std::scoped_lock lock(m_documentsMutex);
        current = m_documents.contains(pending.uri) and
                  m_documents.document(pending.uri).version == pending.version;
        for (const auto &[uri, version] : pending.projectVersions) {
            if (not m_documents.contains(uri) or
                m_documents.document(uri).version != version) {
                current = false;
                break;
            }
        }
    }
    if (not current) {
        for (const PendingSemanticReply &reply : pending.replies) {
            sendError(reply.id, ContentModified,
                      "Document changed before semantic data was ready.");
        }
        return;
    }
    if (not result.errorMessage.empty()) {
        for (const PendingSemanticReply &reply : pending.replies)
            sendError(reply.id, InternalError, result.errorMessage);
        return;
    }
    for (const std::string &warning : result.warnings)
        sendLogMessage(warning, 2);

    CachedSemanticQuery completed;
    completed.version = pending.version;
    completed.positionByte = pending.positionByte;
    completed.text = std::move(result.text);
    completed.hover = std::move(result.hover);
    completed.signatureHelp = std::move(result.signatureHelp);
    completed.definition = std::move(result.definition);
    completed.references = std::move(result.references);
    completed.symbol = std::move(result.symbol);
    completed.projectOccurrences = std::move(result.projectOccurrences);
    completed.projectIndexOccurrences =
        std::move(result.projectIndexOccurrences);
    completed.projectIndexComplete = result.projectIndexComplete;
    completed.projectTexts = std::move(pending.projectTexts);
    completed.projectVersions = std::move(pending.projectVersions);
    {
        std::scoped_lock lock(m_semanticMutex);
        m_semanticCache.insert_or_assign(pending.uri, completed);
    }
    for (const PendingSemanticReply &reply : pending.replies) {
        if (reply.kind == SemanticReplyKind::Rename) {
            std::string error;
            Json edit = renameWorkspaceEdit(
                reply, pending.uri, completed, &error);
            if (not error.empty())
                sendError(reply.id, InvalidParams, error);
            else
                sendResult(reply.id, edit);
        } else {
            sendResult(
                reply.id,
                convertSemanticReply(reply, pending.uri, completed));
        }
    }
}

void BaaLanguageServer::invalidateSymbolRequests(const std::string &uri,
                                                 int code,
                                                 const std::string &message)
{
    struct CancelledRequest
    {
        std::uint64_t token{};
        std::vector<Json> ids;
    };
    std::vector<CancelledRequest> cancelled;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        for (auto it = m_symbolRequests.begin(); it != m_symbolRequests.end();) {
            if (uri.empty() or it->second.uri == uri) {
                cancelled.push_back({it->first, std::move(it->second.ids)});
                it = m_symbolRequests.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const CancelledRequest &request : cancelled) {
        m_compiler.cancelSymbols(request.token);
        for (const Json &id : request.ids) sendError(id, code, message);
    }
}

void BaaLanguageServer::invalidateSemanticRequests(const std::string &uri,
                                                   int code,
                                                   const std::string &message)
{
    struct CancelledRequest
    {
        std::uint64_t token{};
        std::vector<PendingSemanticReply> replies;
    };
    std::vector<CancelledRequest> cancelled;
    {
        std::scoped_lock lock(m_semanticMutex);
        for (auto request = m_semanticRequests.begin();
             request != m_semanticRequests.end();) {
            if (uri.empty() or request->second.uri == uri or
                request->second.projectVersions.contains(uri)) {
                cancelled.push_back(
                    {request->first, std::move(request->second.replies)});
                request = m_semanticRequests.erase(request);
            } else {
                ++request;
            }
        }
    }
    for (const CancelledRequest &request : cancelled) {
        m_compiler.cancelSemantic(request.token);
        for (const PendingSemanticReply &reply : request.replies)
            sendError(reply.id, code, message);
    }
}

void BaaLanguageServer::invalidateCompletionRequests(const std::string &uri,
                                                     int code,
                                                     const std::string &message)
{
    std::vector<Json> ids;
    {
        std::scoped_lock lock(m_completionMutex);
        for (auto it = m_pendingCompletionRequests.begin();
             it != m_pendingCompletionRequests.end();) {
            if (uri.empty() or it->uri == uri) {
                ids.push_back(it->id);
                it = m_pendingCompletionRequests.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Json &id : ids) sendError(id, code, message);
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
