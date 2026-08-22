#include "compiler/BaaCompilerBridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace {
std::string trimmed(std::string value)
{
    const auto isSpace = [](unsigned char character) { return std::isspace(character); };
    value.erase(value.begin(), std::ranges::find_if_not(value, isSpace));
    value.erase(std::ranges::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
                value.end());
    return value;
}

std::filesystem::path pathFromUtf8(std::string_view value)
{
    const auto *first = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path(std::u8string(first, first + value.size()));
}

std::string pathToUtf8(const std::filesystem::path &path)
{
    const std::u8string encoded = path.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

bool nonnegativeSize(const Json &value, std::size_t *result)
{
    if (value.is_number_unsigned()) {
        if (result) *result = value.get<std::size_t>();
        return true;
    }
    if (not value.is_number_integer()) return false;
    const std::int64_t integer = value.get<std::int64_t>();
    if (integer < 0) return false;
    if (result) *result = static_cast<std::size_t>(integer);
    return true;
}

bool positiveInteger(const Json &value)
{
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() > 0;
    return value.is_number_integer() and value.get<std::int64_t>() > 0;
}

bool stringFieldEquals(const Json &object,
                       std::string_view key,
                       std::string_view expected)
{
    const auto field = object.find(std::string(key));
    return field != object.end() and field->is_string() and
           field->get_ref<const std::string &>() == expected;
}

bool validTokenKind(std::string_view kind)
{
    return kind == "type" or kind == "modifier" or kind == "keyword" or
           kind == "identifier" or kind == "number" or kind == "string" or
           kind == "character" or kind == "comment" or kind == "directive" or
           kind == "operator";
}

bool validBaaTokens(std::string_view source, const Json &tokens)
{
    if (not tokens.is_array()) return false;
    std::size_t previousEnd = 0;
    for (const Json &token : tokens) {
        if (not token.is_object() or
            not token.value("kind", Json(nullptr)).is_string() or
            not validTokenKind(token["kind"].get<std::string>()))
            return false;
        const Json span = token.value("span", Json(nullptr));
        if (not span.is_object()) return false;
        const Json start = span.value("start", Json(nullptr));
        const Json end = span.value("end", Json(nullptr));
        if (not start.is_object() or not end.is_object() or
            not positiveInteger(start.value("line", Json(nullptr))) or
            not positiveInteger(start.value("column", Json(nullptr))) or
            not positiveInteger(end.value("line", Json(nullptr))) or
            not positiveInteger(end.value("column", Json(nullptr))))
            return false;
        std::size_t startByte{};
        std::size_t endByte{};
        if (not nonnegativeSize(start.value("byte", Json(nullptr)),
                                &startByte) or
            not nonnegativeSize(end.value("byte", Json(nullptr)),
                                &endByte) or
            startByte < previousEnd or endByte <= startByte or
            endByte > source.size())
            return false;
        if ((startByte < source.size() and
             (static_cast<unsigned char>(source[startByte]) & 0xC0u) ==
                 0x80u) or
            (endByte < source.size() and
             (static_cast<unsigned char>(source[endByte]) & 0xC0u) ==
                 0x80u))
            return false;
        previousEnd = endByte;
    }
    return true;
}

bool validSemanticIdentifierKind(std::string_view kind)
{
    return kind == "function" or kind == "variable" or kind == "constant" or
           kind == "array" or kind == "parameter" or kind == "field" or
           kind == "enum-member" or kind == "type-alias" or kind == "enum" or
           kind == "struct" or kind == "union";
}

bool isUtf8Boundary(std::string_view source, std::size_t byte)
{
    return byte <= source.size() and
           (byte == source.size() or
            (static_cast<unsigned char>(source[byte]) & 0xC0u) != 0x80u);
}

bool structureLocationMatchesByte(std::string_view source,
                                  std::size_t byte,
                                  std::size_t claimedLine,
                                  std::size_t claimedColumn)
{
    std::size_t line = 1;
    std::size_t column = 1;
    std::size_t offset = 0;
    while (offset < byte) {
        if (source[offset] == '\r') {
            if (offset + 1 < byte and source[offset + 1] == '\n') ++offset;
            ++line;
            column = 1;
            ++offset;
            continue;
        }
        if (source[offset] == '\n') {
            ++line;
            column = 1;
            ++offset;
            continue;
        }
        const unsigned char first =
            static_cast<unsigned char>(source[offset]);
        const std::size_t width = first < 0x80u ? 1u
            : (first & 0xE0u) == 0xC0u ? 2u
            : (first & 0xF0u) == 0xE0u ? 3u : 4u;
        if (offset + width > byte) return false;
        offset += width;
        column += width;
    }
    return line == claimedLine and column == claimedColumn;
}

bool appendSemanticIdentifierTokens(std::string_view source,
                                    std::string_view logicalFile,
                                    const Json &occurrences,
                                    Json *tokens)
{
    if (not occurrences.is_array() or not tokens or not tokens->is_array())
        return false;

    const Json rawTokens = *tokens;
    std::set<std::pair<std::size_t, std::size_t>> rawIdentifiers;
    for (const Json &token : rawTokens) {
        if (token.value("kind", "") != "identifier") continue;
        const Json span = token.value("span", Json::object());
        const Json start = span.value("start", Json::object());
        const Json end = span.value("end", Json::object());
        std::size_t startByte{};
        std::size_t endByte{};
        if (not nonnegativeSize(
                start.value("byte", Json(nullptr)), &startByte) or
            not nonnegativeSize(end.value("byte", Json(nullptr)), &endByte))
            return false;
        rawIdentifiers.emplace(startByte, endByte);
    }
    std::map<std::pair<std::size_t, std::size_t>, std::string> emitted;
    for (const Json &occurrence : occurrences) {
        if (not occurrence.is_object()) return false;
        const Json symbol = occurrence.value("symbol", Json(nullptr));
        const Json location = occurrence.value("location", Json(nullptr));
        const Json role = occurrence.value("role", Json(nullptr));
        if (not symbol.is_object() or not location.is_object() or
            not role.is_string() or
            (role != "definition" and role != "declaration" and
             role != "reference") or
            not symbol.value("domain", Json(nullptr)).is_string() or
            not symbol.value("kind", Json(nullptr)).is_string() or
            not symbol.value("name", Json(nullptr)).is_string() or
            not location.value("file", Json(nullptr)).is_string() or
            not location.value("kind", Json(nullptr)).is_string() or
            not location.value("name", Json(nullptr)).is_string())
            return false;

        const std::string kind = symbol["kind"].get<std::string>();
        if (not validSemanticIdentifierKind(kind) or
            location["kind"] != symbol["kind"] or
            location["name"] != symbol["name"])
            return false;
        if (location["file"].get_ref<const std::string &>() != logicalFile)
            continue;

        const Json range = location.value("range", Json(nullptr));
        const Json start = range.is_object()
            ? range.value("start", Json(nullptr)) : Json(nullptr);
        const Json end = range.is_object()
            ? range.value("end", Json(nullptr)) : Json(nullptr);
        if (not start.is_object() or not end.is_object() or
            not positiveInteger(start.value("line", Json(nullptr))) or
            not positiveInteger(start.value("column", Json(nullptr))) or
            not positiveInteger(end.value("line", Json(nullptr))) or
            not positiveInteger(end.value("column", Json(nullptr))))
            return false;

        std::size_t startByte{};
        std::size_t endByte{};
        if (not nonnegativeSize(start.value("byte", Json(nullptr)), &startByte) or
            not nonnegativeSize(end.value("byte", Json(nullptr)), &endByte) or
            endByte <= startByte or endByte > source.size() or
            not isUtf8Boundary(source, startByte) or
            not isUtf8Boundary(source, endByte) or
            not rawIdentifiers.contains(std::pair(startByte, endByte)))
            return false;

        const auto span = std::pair(startByte, endByte);
        const auto previous = emitted.find(span);
        if (previous != emitted.end()) {
            if (previous->second != kind) return false;
            continue;
        }
        emitted.emplace(span, kind);
        tokens->push_back({{"kind", kind}, {"span", range}});
    }
    return true;
}

bool validStructureKind(std::string_view kind, bool folding)
{
    if (folding) return kind == "region" or kind == "comment";
    return kind == "token" or kind == "line" or kind == "content" or
           kind == "group" or kind == "construct" or kind == "document";
}

bool validStructureRanges(std::string_view source,
                          const Json &ranges,
                          bool folding)
{
    if (not ranges.is_array()) return false;
    bool hasPrevious = false;
    std::size_t previousStart{};
    std::size_t previousEnd{};
    std::string previousKind;
    for (const Json &range : ranges) {
        if (not range.is_object() or
            not range.value("kind", Json(nullptr)).is_string() or
            not validStructureKind(
                range["kind"].get_ref<const std::string &>(), folding))
            return false;
        const Json span = range.value("span", Json(nullptr));
        const Json start = span.is_object()
            ? span.value("start", Json(nullptr)) : Json(nullptr);
        const Json end = span.is_object()
            ? span.value("end", Json(nullptr)) : Json(nullptr);
        if (not start.is_object() or not end.is_object())
            return false;
        std::size_t startByte{};
        std::size_t endByte{};
        std::size_t startLine{};
        std::size_t startColumn{};
        std::size_t endLine{};
        std::size_t endColumn{};
        if (not nonnegativeSize(start.value("byte", Json(nullptr)), &startByte) or
            not nonnegativeSize(end.value("byte", Json(nullptr)), &endByte) or
            not nonnegativeSize(start.value("line", Json(nullptr)), &startLine) or
            not nonnegativeSize(
                start.value("column", Json(nullptr)), &startColumn) or
            not nonnegativeSize(end.value("line", Json(nullptr)), &endLine) or
            not nonnegativeSize(end.value("column", Json(nullptr)), &endColumn) or
            startLine == 0 or startColumn == 0 or endLine == 0 or
            endColumn == 0 or
            endByte <= startByte or endByte > source.size() or
            not isUtf8Boundary(source, startByte) or
            not isUtf8Boundary(source, endByte) or
            not structureLocationMatchesByte(
                source, startByte, startLine, startColumn) or
            not structureLocationMatchesByte(
                source, endByte, endLine, endColumn))
            return false;
        if (folding and startLine >= endLine)
            return false;

        const std::string kind = range["kind"].get<std::string>();
        if (hasPrevious and
            (startByte < previousStart or
             (startByte == previousStart and endByte > previousEnd) or
             (startByte == previousStart and endByte == previousEnd and
              kind <= previousKind)))
            return false;
        hasPrevious = true;
        previousStart = startByte;
        previousEnd = endByte;
        previousKind = kind;
    }
    return true;
}

bool validInlayHints(std::string_view source, const Json &hints)
{
    if (not hints.is_array()) return false;
    bool hasPrevious = false;
    std::size_t previousPosition{};
    std::string previousParameter;
    for (const Json &hint : hints) {
        if (not hint.is_object() or
            not stringFieldEquals(hint, "kind", "parameter") or
            not hint.value("label", Json(nullptr)).is_string() or
            not hint.value("parameter", Json(nullptr)).is_string() or
            not hint.value("padding_right", Json(nullptr)).is_boolean())
            return false;
        std::size_t position{};
        if (not nonnegativeSize(
                hint.value("position_byte", Json(nullptr)), &position) or
            not isUtf8Boundary(source, position))
            return false;
        const std::string label = hint["label"].get<std::string>();
        const std::string parameter = hint["parameter"].get<std::string>();
        if (label.empty() or parameter.empty()) return false;
        if (hasPrevious and
            (position < previousPosition or
             (position == previousPosition and parameter < previousParameter)))
            return false;
        hasPrevious = true;
        previousPosition = position;
        previousParameter = parameter;
    }
    return true;
}
}

BaaCompilerBridge::BaaCompilerBridge()
    : m_worker(&BaaCompilerBridge::workerLoop, this)
{
}

BaaCompilerBridge::~BaaCompilerBridge()
{
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
        m_pending.clear();
        m_pendingSymbols.clear();
        m_pendingTokens.clear();
        m_pendingStructure.clear();
        m_pendingInlayHints.clear();
        m_pendingFormats.clear();
        m_pendingSemantic.clear();
        m_completionDataPending = false;
        m_latestVersions.clear();
        m_callback = {};
        m_symbolCallback = {};
        m_tokenCallback = {};
        m_structureCallback = {};
        m_inlayHintCallback = {};
        m_completionDataCallback = {};
        m_formatCallback = {};
        m_semanticCallback = {};
    }
    m_runner.cancel();
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void BaaCompilerBridge::setCompilerProgram(std::string program)
{
    std::scoped_lock lock(m_mutex);
    m_compilerProgram = trimmed(std::move(program));
}

void BaaCompilerBridge::setApplicationDirectory(std::filesystem::path directory)
{
    std::scoped_lock lock(m_mutex);
    m_applicationDirectory = std::move(directory);
}

void BaaCompilerBridge::setDebounceInterval(int milliseconds)
{
    std::scoped_lock lock(m_mutex);
    m_debounce = std::chrono::milliseconds(std::max(0, milliseconds));
}

void BaaCompilerBridge::setAnalysisCallback(AnalysisCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_callback = std::move(callback);
}

void BaaCompilerBridge::setSymbolCallback(SymbolCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_symbolCallback = std::move(callback);
}

void BaaCompilerBridge::setTokenCallback(TokenCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_tokenCallback = std::move(callback);
}

void BaaCompilerBridge::setStructureCallback(StructureCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_structureCallback = std::move(callback);
}

void BaaCompilerBridge::setInlayHintCallback(InlayHintCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_inlayHintCallback = std::move(callback);
}

void BaaCompilerBridge::setCompletionDataCallback(CompletionDataCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_completionDataCallback = std::move(callback);
}

void BaaCompilerBridge::setFormatCallback(FormatCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_formatCallback = std::move(callback);
}

void BaaCompilerBridge::setSemanticCallback(SemanticCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_semanticCallback = std::move(callback);
}

void BaaCompilerBridge::schedule(BaaAnalysisRequest request)
{
    if (not request.isValid()) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.insert_or_assign(request.uri, request.version);
        m_pending.insert_or_assign(request.uri, request);
        std::erase_if(m_pendingSymbols, [&request](const BaaSymbolRequest &symbolRequest) {
            return symbolRequest.uri == request.uri and symbolRequest.version != request.version;
        });
        std::erase_if(m_pendingTokens, [&request](const BaaTokenRequest &tokenRequest) {
            return tokenRequest.uri == request.uri and
                   tokenRequest.version != request.version;
        });
        std::erase_if(
            m_pendingStructure,
            [&request](const BaaStructureRequest &structureRequest) {
                return structureRequest.uri == request.uri and
                       structureRequest.version != request.version;
            });
        std::erase_if(
            m_pendingInlayHints,
            [&request](const BaaInlayHintRequest &inlayRequest) {
                return inlayRequest.uri == request.uri and
                       inlayRequest.version != request.version;
            });
        std::erase_if(m_pendingFormats, [&request](const BaaFormatRequest &formatRequest) {
            return formatRequest.uri == request.uri and
                   formatRequest.version != request.version;
        });
        std::erase_if(m_pendingSemantic, [&request](const BaaSemanticRequest &semanticRequest) {
            return semanticRequest.uri == request.uri and
                   semanticRequest.version != request.version;
        });
        cancelActive = m_activeUri == request.uri and m_activeVersion != request.version;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::requestSymbols(BaaSymbolRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        if (request.requireLatestVersion)
            m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingSymbols.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestTokens(BaaTokenRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingTokens.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestStructure(BaaStructureRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingStructure.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestInlayHints(BaaInlayHintRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingInlayHints.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestCompletionData()
{
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_completionDataPending = true;
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestFormat(BaaFormatRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingFormats.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestSemantic(BaaSemanticRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingSemantic.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelSymbols(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_pendingSymbols, [token](const BaaSymbolRequest &request) {
            return request.token == token;
        });
        cancelActive = m_activeSymbolToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelTokens(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_pendingTokens, [token](const BaaTokenRequest &request) {
            return request.token == token;
        });
        cancelActive = m_activeTokenToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelStructure(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(
            m_pendingStructure,
            [token](const BaaStructureRequest &request) {
                return request.token == token;
            });
        cancelActive = m_activeStructureToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelInlayHints(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(
            m_pendingInlayHints,
            [token](const BaaInlayHintRequest &request) {
                return request.token == token;
            });
        cancelActive = m_activeInlayHintToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelFormat(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_pendingFormats, [token](const BaaFormatRequest &request) {
            return request.token == token;
        });
        cancelActive = m_activeFormatToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelSemantic(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_pendingSemantic, [token](const BaaSemanticRequest &request) {
            return request.token == token;
        });
        cancelActive = m_activeSemanticToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancel(const std::string &uri)
{
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        m_pending.erase(uri);
        std::erase_if(m_pendingSymbols, [&uri](const BaaSymbolRequest &request) {
            return request.uri == uri;
        });
        std::erase_if(m_pendingTokens, [&uri](const BaaTokenRequest &request) {
            return request.uri == uri;
        });
        std::erase_if(
            m_pendingStructure,
            [&uri](const BaaStructureRequest &request) {
                return request.uri == uri;
            });
        std::erase_if(
            m_pendingInlayHints,
            [&uri](const BaaInlayHintRequest &request) {
                return request.uri == uri;
            });
        std::erase_if(m_pendingFormats, [&uri](const BaaFormatRequest &request) {
            return request.uri == uri;
        });
        std::erase_if(m_pendingSemantic, [&uri](const BaaSemanticRequest &request) {
            return request.uri == uri;
        });
        m_latestVersions.erase(uri);
        cancelActive = m_activeUri == uri;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelAll()
{
    bool hasActive = false;
    {
        std::scoped_lock lock(m_mutex);
        m_pending.clear();
        m_pendingSymbols.clear();
        m_pendingTokens.clear();
        m_pendingStructure.clear();
        m_pendingInlayHints.clear();
        m_pendingFormats.clear();
        m_pendingSemantic.clear();
        m_completionDataPending = false;
        m_latestVersions.clear();
        hasActive = not m_activeUri.empty() or m_completionDataActive;
        ++m_scheduleSerial;
    }
    if (hasActive) m_runner.cancel();
    m_wake.notify_one();
}

std::string BaaCompilerBridge::resolveCompilerProgram() const
{
    std::string configured;
    std::filesystem::path applicationDirectory;
    {
        std::scoped_lock lock(m_mutex);
        configured = m_compilerProgram;
        applicationDirectory = m_applicationDirectory;
    }
    if (configured.empty()) {
        if (const char *environment = std::getenv("BAA")) configured = trimmed(environment);
    }
    if (not configured.empty()) return configured;

#if defined(_WIN32)
    constexpr std::string_view executable = "baa.exe";
#else
    constexpr std::string_view executable = "baa";
#endif
    if (not applicationDirectory.empty()) {
        const std::filesystem::path nested = applicationDirectory / "baa" / executable;
        if (std::filesystem::is_regular_file(nested)) return pathToUtf8(nested);
        const std::filesystem::path adjacent = applicationDirectory / executable;
        if (std::filesystem::is_regular_file(adjacent)) return pathToUtf8(adjacent);
    }
    return std::string(executable);
}

void BaaCompilerBridge::workerLoop()
{
    while (true) {
        BaaAnalysisRequest request;
        BaaSymbolRequest symbolRequest;
        BaaTokenRequest tokenRequest;
        BaaStructureRequest structureRequest;
        BaaInlayHintRequest inlayHintRequest;
        BaaFormatRequest formatRequest;
        BaaSemanticRequest semanticRequest;
        bool isSymbolRequest = false;
        bool isTokenRequest = false;
        bool isStructureRequest = false;
        bool isInlayHintRequest = false;
        bool isFormatRequest = false;
        bool isSemanticRequest = false;
        bool isCompletionDataRequest = false;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_stopping or m_completionDataPending or
                       not m_pendingFormats.empty() or
                       not m_pendingSemantic.empty() or
                       not m_pendingInlayHints.empty() or
                       not m_pendingStructure.empty() or
                       not m_pendingTokens.empty() or
                       not m_pendingSymbols.empty() or not m_pending.empty();
            });
            if (m_stopping) return;

            if (not m_completionDataPending and m_pendingFormats.empty() and
                m_pendingSemantic.empty() and
                m_pendingInlayHints.empty() and
                m_pendingStructure.empty() and
                m_pendingTokens.empty() and
                m_pendingSymbols.empty()) {
                const std::uint64_t observedSerial = m_scheduleSerial;
                const std::chrono::milliseconds debounce = m_debounce;
                if (m_wake.wait_for(lock, debounce, [this, observedSerial] {
                        return m_stopping or m_scheduleSerial != observedSerial;
                    })) {
                    if (m_stopping) return;
                    continue;
                }
            }

            if (m_completionDataPending) {
                isCompletionDataRequest = true;
                m_completionDataPending = false;
                m_completionDataActive = true;
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeSymbolToken = 0;
                m_activeTokenToken = 0;
                m_activeStructureToken = 0;
                m_activeInlayHintToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            } else if (not m_pendingFormats.empty()) {
                isFormatRequest = true;
                formatRequest = std::move(m_pendingFormats.front());
                m_pendingFormats.pop_front();
                m_activeUri = formatRequest.uri;
                m_activeVersion = formatRequest.version;
                m_activeSymbolToken = 0;
                m_activeTokenToken = 0;
                m_activeStructureToken = 0;
                m_activeInlayHintToken = 0;
                m_activeFormatToken = formatRequest.token;
                m_activeSemanticToken = 0;
            } else if (not m_pendingSemantic.empty()) {
                isSemanticRequest = true;
                semanticRequest = std::move(m_pendingSemantic.front());
                m_pendingSemantic.pop_front();
                m_activeUri = semanticRequest.uri;
                m_activeVersion = semanticRequest.version;
                m_activeSymbolToken = 0;
                m_activeTokenToken = 0;
                m_activeStructureToken = 0;
                m_activeInlayHintToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = semanticRequest.token;
            } else if (not m_pendingInlayHints.empty()) {
                isInlayHintRequest = true;
                inlayHintRequest = std::move(m_pendingInlayHints.front());
                m_pendingInlayHints.pop_front();
                m_activeUri = inlayHintRequest.uri;
                m_activeVersion = inlayHintRequest.version;
                m_activeSymbolToken = 0;
                m_activeTokenToken = 0;
                m_activeStructureToken = 0;
                m_activeInlayHintToken = inlayHintRequest.token;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            } else if (not m_pendingStructure.empty()) {
                isStructureRequest = true;
                structureRequest = std::move(m_pendingStructure.front());
                m_pendingStructure.pop_front();
                m_activeUri = structureRequest.uri;
                m_activeVersion = structureRequest.version;
                m_activeSymbolToken = 0;
                m_activeTokenToken = 0;
                m_activeStructureToken = structureRequest.token;
                m_activeInlayHintToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            } else if (not m_pendingTokens.empty()) {
                isTokenRequest = true;
                tokenRequest = std::move(m_pendingTokens.front());
                m_pendingTokens.pop_front();
                m_activeUri = tokenRequest.uri;
                m_activeVersion = tokenRequest.version;
                m_activeSymbolToken = 0;
                m_activeTokenToken = tokenRequest.token;
                m_activeStructureToken = 0;
                m_activeInlayHintToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            } else if (not m_pendingSymbols.empty()) {
                isSymbolRequest = true;
                symbolRequest = std::move(m_pendingSymbols.front());
                m_pendingSymbols.pop_front();
                m_activeUri = symbolRequest.uri;
                m_activeVersion = symbolRequest.version;
                m_activeSymbolToken = symbolRequest.token;
                m_activeTokenToken = 0;
                m_activeStructureToken = 0;
                m_activeInlayHintToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            } else {
                if (m_pending.empty()) continue;
                auto next = m_pending.begin();
                request = std::move(next->second);
                m_pending.erase(next);
                m_activeUri = request.uri;
                m_activeVersion = request.version;
                m_activeSymbolToken = 0;
                m_activeTokenToken = 0;
                m_activeStructureToken = 0;
                m_activeInlayHintToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            }
            m_runner.prepare();
        }

        const std::string compiler = resolveCompilerProgram();
        if (isCompletionDataRequest) {
            ProcessResult process = m_runner.run(
                compiler, {"--completion-data=json"}, {}, {});
            BaaCompletionDataResult result;
            result.exitCode = process.exitCode;
            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(process.standardOutput, nullptr, false);
                if (not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "completion-data-json-v1" and
                    parsed.value("language", "") == "baa" and
                    parsed.value("items", Json::array()).is_array()) {
                    result.items = parsed["items"];
                } else {
                    result.errorMessage =
                        "Baa returned completion data that does not satisfy completion-data-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " + trimmed(process.standardError);
                    }
                }
            }

            CompletionDataCallback callback;
            {
                std::scoped_lock lock(m_mutex);
                m_completionDataActive = false;
                callback = m_completionDataCallback;
            }
            if (not process.cancelled and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        const std::string &filePath = isFormatRequest
            ? formatRequest.filePath
            : isSemanticRequest
            ? semanticRequest.filePath
            : isInlayHintRequest
            ? inlayHintRequest.filePath
            : isStructureRequest
            ? structureRequest.filePath
            : isTokenRequest
            ? tokenRequest.filePath
            : (isSymbolRequest ? symbolRequest.filePath : request.filePath);
        const std::string &text = isFormatRequest
            ? formatRequest.text
            : isSemanticRequest
            ? semanticRequest.text
            : isInlayHintRequest
            ? inlayHintRequest.text
            : isStructureRequest
            ? structureRequest.text
            : isTokenRequest
            ? tokenRequest.text
            : (isSymbolRequest ? symbolRequest.text : request.text);
        const std::filesystem::path sourcePath = pathFromUtf8(filePath);
        std::vector<std::string> arguments;
        if (isFormatRequest) {
            arguments = {"--format=json", "--source-stdin=" + filePath};
        } else if (isInlayHintRequest) {
            arguments = {"--inlay-hints=json"};
            for (const std::string &includePath : inlayHintRequest.includePaths) {
                arguments.push_back("-I");
                arguments.push_back(includePath);
            }
            arguments.push_back("--source-stdin=" + filePath);
        } else if (isStructureRequest) {
            arguments = {
                "--dump-structure=json", "--source-stdin=" + filePath
            };
        } else if (isTokenRequest) {
            arguments = {"--dump-tokens=json", "--source-stdin=" + filePath};
        } else if (isSemanticRequest) {
            arguments = {
                "--semantic-query=json",
                "--position-byte=" + std::to_string(semanticRequest.positionByte)
            };
            for (const std::string &includePath : semanticRequest.includePaths) {
                arguments.push_back("-I");
                arguments.push_back(includePath);
            }
            arguments.push_back("--source-stdin=" + filePath);
        } else if (isSymbolRequest) {
            arguments = {"--dump-symbols=json"};
            for (const std::string &includePath : symbolRequest.includePaths) {
                arguments.push_back("-I");
                arguments.push_back(includePath);
            }
            arguments.push_back("--source-stdin=" + filePath);
        } else {
            arguments = {"--check", "--diagnostics=json",
                         "--source-stdin=" + filePath};
        }
        const std::filesystem::path workingDirectory =
            isSemanticRequest and
            not semanticRequest.projectWorkingDirectory.empty()
                ? semanticRequest.projectWorkingDirectory
                : isInlayHintRequest and
                  not inlayHintRequest.projectWorkingDirectory.empty()
                    ? inlayHintRequest.projectWorkingDirectory
                : isTokenRequest and
                  not tokenRequest.projectWorkingDirectory.empty()
                    ? tokenRequest.projectWorkingDirectory
                    : sourcePath.parent_path();
        ProcessResult process = m_runner.run(
            compiler, arguments, workingDirectory, text);

        if (isInlayHintRequest) {
            BaaInlayHintResult result;
            result.token = inlayHintRequest.token;
            result.uri = inlayHintRequest.uri;
            result.text = inlayHintRequest.text;
            result.version = inlayHintRequest.version;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(
                    process.standardOutput, nullptr, false);
                const Json hints = parsed.is_object()
                    ? parsed.value("hints", Json(nullptr))
                    : Json(nullptr);
                if (process.exitCode == 0 and
                    not parsed.is_discarded() and parsed.is_object() and
                    stringFieldEquals(
                        parsed, "schema_version", "inlay-hints-json-v1") and
                    parsed.value(
                        "compiler_version", Json(nullptr)).is_string() and
                    stringFieldEquals(parsed, "file", filePath) and
                    stringFieldEquals(
                        parsed, "position_encoding", "utf-8-bytes") and
                    parsed.value("complete", Json(nullptr)).is_boolean() and
                    validInlayHints(text, hints)) {
                    result.complete = parsed["complete"].get<bool>();
                    result.hints = hints;
                } else {
                    result.errorMessage =
                        "Baa returned inlay hints that do not satisfy "
                        "inlay-hints-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " +
                            trimmed(process.standardError);
                    }
                }
            }

            InlayHintCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeInlayHintToken = 0;
                const auto latest =
                    m_latestVersions.find(inlayHintRequest.uri);
                publish = not process.cancelled and
                          latest != m_latestVersions.end() and
                          latest->second == inlayHintRequest.version;
                callback = m_inlayHintCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        if (isStructureRequest) {
            BaaStructureResult result;
            result.token = structureRequest.token;
            result.uri = structureRequest.uri;
            result.text = structureRequest.text;
            result.version = structureRequest.version;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(
                    process.standardOutput, nullptr, false);
                std::size_t sourceBytes = 0;
                const Json foldingRanges = parsed.is_object()
                    ? parsed.value("folding_ranges", Json(nullptr))
                    : Json(nullptr);
                const Json selectionRanges = parsed.is_object()
                    ? parsed.value("selection_ranges", Json(nullptr))
                    : Json(nullptr);
                if (process.exitCode == 0 and
                    not parsed.is_discarded() and parsed.is_object() and
                    stringFieldEquals(
                        parsed, "schema_version", "structure-json-v1") and
                    parsed.value(
                        "compiler_version", Json(nullptr)).is_string() and
                    stringFieldEquals(parsed, "language", "baa") and
                    stringFieldEquals(parsed, "file", filePath) and
                    stringFieldEquals(
                        parsed, "position_encoding", "utf-8-bytes") and
                    nonnegativeSize(
                        parsed.value("source_bytes", Json(nullptr)),
                        &sourceBytes) and
                    sourceBytes == text.size() and
                    parsed.value("complete", Json(nullptr)).is_boolean() and
                    validStructureRanges(text, foldingRanges, true) and
                    validStructureRanges(text, selectionRanges, false)) {
                    result.complete = parsed["complete"].get<bool>();
                    result.foldingRanges = foldingRanges;
                    result.selectionRanges = selectionRanges;
                } else {
                    result.errorMessage =
                        "Baa returned structure data that does not satisfy "
                        "structure-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " +
                            trimmed(process.standardError);
                    }
                }
            }

            StructureCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeStructureToken = 0;
                const auto latest = m_latestVersions.find(structureRequest.uri);
                publish = not process.cancelled and
                          latest != m_latestVersions.end() and
                          latest->second == structureRequest.version;
                callback = m_structureCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        if (isTokenRequest) {
            BaaTokenResult result;
            result.token = tokenRequest.token;
            result.uri = tokenRequest.uri;
            result.text = tokenRequest.text;
            result.version = tokenRequest.version;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(
                    process.standardOutput, nullptr, false);
                std::size_t sourceBytes = 0;
                const Json rawTokens = parsed.is_object()
                    ? parsed.value("tokens", Json(nullptr))
                    : Json(nullptr);
                if (process.exitCode == 0 and
                    not parsed.is_discarded() and parsed.is_object() and
                    stringFieldEquals(
                        parsed, "schema_version", "tokens-json-v1") and
                    parsed.value("compiler_version", Json(nullptr)).is_string() and
                    stringFieldEquals(parsed, "language", "baa") and
                    parsed.value("file", Json(nullptr)).is_string() and
                    stringFieldEquals(
                        parsed, "position_encoding", "utf-8-bytes") and
                    nonnegativeSize(
                        parsed.value("source_bytes", Json(nullptr)),
                        &sourceBytes) and
                    sourceBytes == text.size() and
                    validBaaTokens(text, rawTokens)) {
                    result.tokens = rawTokens;

                    std::vector<std::string> indexArguments{
                        "--semantic-index=json"
                    };
                    for (const std::string &includePath :
                         tokenRequest.includePaths) {
                        indexArguments.push_back("-I");
                        indexArguments.push_back(includePath);
                    }
                    indexArguments.push_back(
                        "--source-stdin=" + tokenRequest.filePath);
                    ProcessResult indexProcess = m_runner.run(
                        compiler, indexArguments, workingDirectory,
                        tokenRequest.text);
                    if (indexProcess.cancelled) {
                        process.cancelled = true;
                    } else if (not indexProcess.started) {
                        result.errorMessage =
                            indexProcess.errorMessage.empty()
                                ? "Baa compiler executable was not found."
                                : indexProcess.errorMessage;
                    } else if (indexProcess.exitCode == 1) {
                        // Incomplete source keeps the compiler-owned lexical
                        // tokens and simply has no analyzed identifier roles.
                    } else if (indexProcess.exitCode != 0) {
                        result.errorMessage =
                            "Baa semantic index failed with exit code " +
                            std::to_string(indexProcess.exitCode) + ".";
                        if (not indexProcess.standardError.empty()) {
                            result.errorMessage += " " +
                                trimmed(indexProcess.standardError);
                        }
                    } else {
                        const Json index = Json::parse(
                            indexProcess.standardOutput, nullptr, false);
                        const Json occurrences = index.is_object()
                            ? index.value("occurrences", Json(nullptr))
                            : Json(nullptr);
                        if (index.is_discarded() or not index.is_object() or
                            not stringFieldEquals(
                                index, "schema_version",
                                "semantic-index-json-v1") or
                            not index.value(
                                "compiler_version", Json(nullptr)).is_string() or
                            not index.value("file", Json(nullptr)).is_string() or
                            not stringFieldEquals(
                                index, "position_encoding", "utf-8-bytes") or
                            not appendSemanticIdentifierTokens(
                                tokenRequest.text,
                                index["file"].get_ref<const std::string &>(),
                                occurrences, &result.tokens)) {
                            result.errorMessage =
                                "Baa returned identifier roles that do not "
                                "satisfy semantic-index-json-v1.";
                            if (not indexProcess.standardError.empty()) {
                                result.errorMessage += " " +
                                    trimmed(indexProcess.standardError);
                            }
                        }
                    }
                } else {
                    result.errorMessage =
                        "Baa returned tokens that do not satisfy tokens-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " +
                            trimmed(process.standardError);
                    }
                }
            }

            TokenCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeTokenToken = 0;
                const auto latest = m_latestVersions.find(tokenRequest.uri);
                publish = not process.cancelled and
                          latest != m_latestVersions.end() and
                          latest->second == tokenRequest.version;
                callback = m_tokenCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        if (isFormatRequest) {
            BaaFormatResult result;
            result.token = formatRequest.token;
            result.uri = formatRequest.uri;
            result.text = formatRequest.text;
            result.version = formatRequest.version;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(
                    process.standardOutput, nullptr, false);
                if (process.exitCode == 0 and
                    not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "format-json-v1" and
                    parsed.value("compiler_version", Json(nullptr)).is_string() and
                    parsed.value("language", "") == "baa" and
                    parsed.value("file", Json(nullptr)).is_string() and
                    parsed.value("position_encoding", "") == "utf-8-bytes" and
                    parsed.value("line_ending", "") == "lf" and
                    parsed.value("indent_width", 0) == 4 and
                    parsed.value("insert_spaces", false) and
                    parsed.value("source_bytes", std::size_t{}) == text.size() and
                    parsed.value("formatted_bytes", Json(nullptr))
                        .is_number_unsigned() and
                    parsed.value("changed", Json(nullptr)).is_boolean() and
                    parsed.value("formatted_text", Json(nullptr)).is_string()) {
                    result.formattedText =
                        parsed["formatted_text"].get<std::string>();
                    result.changed = parsed["changed"].get<bool>();
                    const bool exactMetadata =
                        parsed["formatted_bytes"].get<std::size_t>() ==
                            result.formattedText.size() and
                        result.changed == (result.formattedText != text);
                    if (not exactMetadata) {
                        result.errorMessage =
                            "Baa returned formatting byte/change metadata that "
                            "does not match format-json-v1.";
                    }
                } else {
                    result.errorMessage =
                        "Baa returned formatting data that does not satisfy "
                        "format-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " +
                            trimmed(process.standardError);
                    }
                }
            }

            FormatCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeFormatToken = 0;
                const auto latest = m_latestVersions.find(formatRequest.uri);
                publish = not process.cancelled and
                          latest != m_latestVersions.end() and
                          latest->second == formatRequest.version;
                callback = m_formatCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        if (isSemanticRequest) {
            BaaSemanticResult result;
            result.token = semanticRequest.token;
            result.uri = semanticRequest.uri;
            result.text = semanticRequest.text;
            result.version = semanticRequest.version;
            result.positionByte = semanticRequest.positionByte;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled and process.exitCode == 1) {
                // An incomplete/invalid edit has no current semantic answer.
                // Return null LSP results instead of turning a source error into
                // a language-server transport failure.
                result.hover = nullptr;
                result.signatureHelp = nullptr;
                result.definition = nullptr;
                result.references = Json::array();
                result.completionItems = Json::array();
                result.completionComplete = false;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(process.standardOutput, nullptr, false);
                if (not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "semantic-query-json-v1" and
                    parsed.value("position_encoding", "") == "utf-8-bytes" and
                    parsed.value("position_byte", std::size_t{}) ==
                        semanticRequest.positionByte and
                    parsed.contains("symbol") and
                    parsed.contains("hover") and
                    parsed.contains("signature_help") and
                    parsed.contains("definition") and
                    parsed.contains("references") and
                    parsed["references"].is_array() and
                    parsed.contains("completion") and
                    parsed["completion"].is_object() and
                    parsed["completion"].contains("items") and
                    parsed["completion"]["items"].is_array()) {
                    result.hover = parsed["hover"];
                    result.signatureHelp = parsed["signature_help"];
                    result.definition = parsed["definition"];
                    result.references = parsed["references"];
                    result.completionItems =
                        parsed["completion"]["items"];
                    result.completionComplete = true;
                    result.symbol = parsed["symbol"];
                } else {
                    result.errorMessage =
                        "Baa returned semantic data that does not satisfy "
                        "semantic-query-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " + trimmed(process.standardError);
                    }
                }
            }

            if (result.errorMessage.empty() and
                not process.cancelled and result.symbol.is_object() and
                semanticRequest.projectIndexRequired and
                not semanticRequest.projectSources.empty()) {
                const std::string domain = result.symbol.value("domain", "");
                const bool projectIdentity =
                    domain == "external" or domain == "declaration";
                // All project translation units participate in an
                // external/declaration identity. Local and file identities
                // only need the origin unit, but still require its complete
                // compiler-owned index for collision-checked rename.
                for (const BaaSemanticRequest::ProjectSource &projectSource :
                     semanticRequest.projectSources) {
                    if (not projectIdentity and
                        projectSource.filePath != semanticRequest.filePath)
                        continue;
                    std::vector<std::string> indexArguments{
                        "--semantic-index=json"
                    };
                    for (const std::string &includePath :
                         semanticRequest.includePaths) {
                        indexArguments.push_back("-I");
                        indexArguments.push_back(includePath);
                    }
                    if (projectSource.useStandardInput) {
                        indexArguments.push_back(
                            "--source-stdin=" + projectSource.filePath);
                    } else {
                        indexArguments.push_back(projectSource.filePath);
                    }

                    const std::filesystem::path projectPath =
                        pathFromUtf8(projectSource.filePath);
                    const std::filesystem::path indexWorkingDirectory =
                        not semanticRequest.projectWorkingDirectory.empty()
                            ? semanticRequest.projectWorkingDirectory
                            : projectPath.parent_path();
                    ProcessResult indexProcess = m_runner.run(
                        compiler,
                        indexArguments,
                        indexWorkingDirectory,
                        projectSource.useStandardInput
                            ? std::string_view(projectSource.text)
                            : std::string_view{});
                    if (indexProcess.cancelled) {
                        process.cancelled = true;
                        break;
                    }
                    if (not indexProcess.started) {
                        result.errorMessage =
                            indexProcess.errorMessage.empty()
                                ? "Baa compiler executable was not found."
                                : indexProcess.errorMessage;
                        break;
                    }
                    if (indexProcess.exitCode == 1) {
                        result.projectIndexComplete = false;
                        result.warnings.push_back(
                            "Project semantic index skipped a source with "
                            "compiler errors: " + projectSource.filePath);
                        continue;
                    }
                    if (indexProcess.exitCode != 0) {
                        result.errorMessage =
                            "Baa semantic index failed for " +
                            projectSource.filePath + " with exit code " +
                            std::to_string(indexProcess.exitCode) + ".";
                        if (not indexProcess.standardError.empty()) {
                            result.errorMessage += " " +
                                trimmed(indexProcess.standardError);
                        }
                        break;
                    }

                    const Json index = Json::parse(
                        indexProcess.standardOutput, nullptr, false);
                    if (index.is_discarded() or not index.is_object() or
                        index.value("schema_version", "") !=
                            "semantic-index-json-v1" or
                        index.value("position_encoding", "") !=
                            "utf-8-bytes" or
                        not index.value(
                            "occurrences", Json::array()).is_array()) {
                        result.errorMessage =
                            "Baa returned semantic index data that does not "
                            "satisfy semantic-index-json-v1 for " +
                            projectSource.filePath + ".";
                        break;
                    }
                    for (const Json &occurrence : index["occurrences"]) {
                        if (occurrence.is_object())
                            result.projectIndexOccurrences.push_back(
                                occurrence);
                        if (occurrence.is_object() and
                            occurrence.value("symbol", Json(nullptr)) ==
                                result.symbol and
                            occurrence.contains("location") and
                            occurrence["location"].is_object()) {
                            result.projectOccurrences.push_back(occurrence);
                        }
                    }
                }
            }

            SemanticCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeSemanticToken = 0;
                m_activeFormatToken = 0;
                const auto latest = m_latestVersions.find(semanticRequest.uri);
                publish = not process.cancelled and latest != m_latestVersions.end() and
                          latest->second == semanticRequest.version;
                callback = m_semanticCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        if (isSymbolRequest) {
            BaaSymbolResult result;
            result.token = symbolRequest.token;
            result.uri = symbolRequest.uri;
            result.text = symbolRequest.text;
            result.version = symbolRequest.version;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled and process.exitCode == 1) {
                // An incomplete editor buffer has no stable outline. This is
                // an ordinary source result, not a symbols-json failure.
                result.symbols = Json::array();
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(process.standardOutput, nullptr, false);
                if (not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "symbols-json-v1" and
                    parsed.value("position_encoding", "") == "utf-8-bytes" and
                    parsed.value("symbols", Json::array()).is_array()) {
                    result.symbols = parsed["symbols"];
                } else {
                    result.errorMessage =
                        "Baa returned symbols that do not satisfy symbols-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " + trimmed(process.standardError);
                    }
                }
            }

            SymbolCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeSymbolToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
                const auto latest = m_latestVersions.find(symbolRequest.uri);
                publish = not process.cancelled and
                          (not symbolRequest.requireLatestVersion or
                           (latest != m_latestVersions.end() and
                            latest->second == symbolRequest.version));
                callback = m_symbolCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        BaaAnalysisResult result;
        result.uri = request.uri;
        result.filePath = request.filePath;
        result.text = request.text;
        result.version = request.version;
        result.exitCode = process.exitCode;

        if (not process.started) {
            result.errorMessage = process.errorMessage.empty()
                ? "Baa compiler executable was not found."
                : process.errorMessage;
        } else if (not process.cancelled) {
            const Json parsed = Json::parse(process.standardOutput, nullptr, false);
            if (not parsed.is_discarded() and parsed.is_object() and
                parsed.value("schema_version", "") == "diagnostics-json-v1" and
                parsed.value("diagnostics", Json::array()).is_array()) {
                result.diagnostics = parsed["diagnostics"];
            } else {
                result.errorMessage = "Baa returned diagnostics that do not satisfy diagnostics-json-v1.";
                if (not process.standardError.empty()) {
                    result.errorMessage += " " + trimmed(process.standardError);
                }
            }
        }

        AnalysisCallback callback;
        bool publish = false;
        {
            std::scoped_lock lock(m_mutex);
            m_activeUri.clear();
            m_activeVersion = 0;
            m_activeSymbolToken = 0;
            m_activeFormatToken = 0;
            m_activeSemanticToken = 0;
            const auto latest = m_latestVersions.find(request.uri);
            publish = not process.cancelled and latest != m_latestVersions.end() and
                      latest->second == request.version;
            callback = m_callback;
        }
        if (publish and callback) callback(std::move(result));
        m_wake.notify_one();
    }
}
