#include "server/BaaLanguageServer.h"

#include "lsp/PositionEncoding.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
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
constexpr std::string_view StructuredLogSchema = "baa-lsp-log-v1";

std::string_view logSeverity(int type)
{
    switch (type) {
    case 1: return "error";
    case 2: return "warning";
    case 3: return "info";
    default: return "debug";
    }
}

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

bool nonnegativeIntValue(const Json &value, int *result)
{
    if (not result) return false;
    if (value.is_number_unsigned()) {
        const std::uint64_t integer = value.get<std::uint64_t>();
        if (integer > static_cast<std::uint64_t>(
                std::numeric_limits<int>::max()))
            return false;
        *result = static_cast<int>(integer);
        return true;
    }
    if (not value.is_number_integer()) return false;
    const std::int64_t integer = value.get<std::int64_t>();
    if (integer < 0 or integer > std::numeric_limits<int>::max()) return false;
    *result = static_cast<int>(integer);
    return true;
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

bool pathIsWithin(const std::filesystem::path &path,
                  const std::filesystem::path &root)
{
    const std::string candidate = comparablePath(path);
    const std::string parent = comparablePath(root);
    if (candidate.empty() or parent.empty() or
        not candidate.starts_with(parent))
        return false;
    if (candidate.size() == parent.size()) return true;
    if (parent.back() == '/' or parent.back() == '\\') return true;
    const char boundary = candidate[parent.size()];
    return boundary == '/' or boundary == '\\';
}

std::optional<std::size_t> lspByteOffset(std::string_view text,
                                         const Json &position)
{
    if (not position.is_object()) return std::nullopt;
    const auto lineIt = position.find("line");
    const auto characterIt = position.find("character");
    int line{};
    int character{};
    if (lineIt == position.end() or characterIt == position.end() or
        not nonnegativeIntValue(*lineIt, &line) or
        not nonnegativeIntValue(*characterIt, &character))
        return std::nullopt;
    const std::size_t offset =
        PositionEncoding::utf8ByteOffsetForUtf16Position(text, line, character);
    const Json canonical{{"line", line}, {"character", character}};
    if (PositionEncoding::utf16PositionForByteOffset(text, offset) != canonical)
        return std::nullopt;
    return offset;
}

std::optional<std::pair<std::size_t, std::size_t>>
lspByteRange(std::string_view text, const Json &range)
{
    if (not range.is_object()) return std::nullopt;
    const auto start = lspByteOffset(text, objectValue(range, "start"));
    const auto end = lspByteOffset(text, objectValue(range, "end"));
    if (not start or not end or *end < *start) return std::nullopt;
    return std::pair{*start, *end};
}

bool lspRangesIntersect(const std::pair<std::size_t, std::size_t> &left,
                        const std::pair<std::size_t, std::size_t> &right)
{
    return left.first <= right.second and right.first <= left.second;
}

Json inlayHintsForRange(std::string_view text,
                        const Json &hints,
                        std::size_t startByte,
                        std::size_t endByte,
                        bool complete)
{
    Json converted = Json::array();
    if (not hints.is_array()) return converted;
    for (const Json &hint : hints) {
        const auto position = hint.find("position_byte");
        if (position == hint.end()) continue;
        std::size_t byte{};
        if (position->is_number_unsigned()) {
            byte = position->get<std::size_t>();
        } else if (position->is_number_integer()) {
            const std::int64_t value = position->get<std::int64_t>();
            if (value < 0) continue;
            byte = static_cast<std::size_t>(value);
        } else {
            continue;
        }
        if (byte < startByte or byte > endByte or byte > text.size()) continue;
        converted.push_back({
            {"position", PositionEncoding::utf16PositionForByteOffset(text, byte)},
            {"label", hint.value("label", "")},
            {"kind", 2},
            {"paddingRight", hint.value("padding_right", false)},
            {"data", {
                {"schema_version", "inlay-hints-json-v1"},
                {"parameter", hint.value("parameter", "")},
                {"complete", complete}
            }}
        });
    }
    return converted;
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
        "#", "_", "\"", "/", "\\", "ا", "أ", "إ", "آ", "ب", "ت", "ث", "ج", "ح", "خ",
        "د", "ذ", "ر", "ز", "س", "ش", "ص", "ض", "ط", "ظ", "ع", "غ",
        "ف", "ق", "ك", "ل", "م", "ن", "ه", "و", "ي", "ى", "ة", "ؤ",
        "ئ", "ء"
    });
}

bool isBaaImplementationPath(const std::filesystem::path &path)
{
    const std::filesystem::path extension = path.extension();
    return extension == pathFromUtf8(".باء") or extension == ".baa";
}

bool isBaaDocumentPath(const std::filesystem::path &path)
{
    const std::filesystem::path extension = path.extension();
    return isBaaImplementationPath(path) or
           extension == pathFromUtf8(".رأسباء") or extension == ".baahd";
}

int metadataCompletionKind(std::string_view kind)
{
    if (kind == "snippet") return 15;
    if (kind == "value") return 12;
    if (kind == "type") return 7;
    if (kind == "function") return 3;
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

    if (not directiveContext and symbols.is_array()) {
        for (const Json &symbol : symbols) {
            if (not symbol.is_object()) continue;
            const std::string name = symbol.value(
                "label", symbol.value("name", ""));
            const std::string kind = symbol.value("kind", "");
            if (name.empty() or not completionMatches(name, prefix)) continue;
            if (not seen.insert(name).second) continue;
            Json item{
                {"label", name},
                {"kind", symbolCompletionKind(kind)},
                {"filterText", symbol.value("filter_text", name)},
                {"insertTextFormat", 1},
                {"sortText", "0" + name},
                {"textEdit", {
                    {"range", replacementRange},
                    {"newText", symbol.value("insert_text", name)}
                }}
            };
            const std::string explicitDetail = symbol.value("detail", "");
            const std::string detail = explicitDetail.empty()
                ? completionSymbolDetail(symbol) : explicitDetail;
            if (not detail.empty()) item["detail"] = detail;
            const std::string documentation =
                symbol.value("documentation", "");
            if (not documentation.empty()) {
                item["data"] = {
                    {"source", "baa-compiler"},
                    {"documentation", documentation}
                };
            }
            result.push_back(std::move(item));
        }
    }

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
            if (not seen.insert(label).second) continue;

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
            const std::string documentation =
                source.value("documentation", "");
            if (not documentation.empty()) {
                item["data"] = {
                    {"source", "baa-compiler"},
                    {"documentation", documentation}
                };
            }
            result.push_back(std::move(item));
        }
    }
    return result;
}

bool isIncludeCandidate(const std::filesystem::directory_entry &entry)
{
    std::error_code error;
    if (entry.is_directory(error)) return not error;
    return not error and entry.is_regular_file(error) and not error and
           isBaaDocumentPath(entry.path());
}

std::optional<Json> buildIncludePathCompletion(
    std::string_view text,
    std::size_t cursor,
    const std::vector<std::filesystem::path> &searchRoots)
{
    cursor = std::min(cursor, text.size());
    const std::size_t lineStart = cursor == 0 ? 0 : text.rfind('\n', cursor - 1) + 1;
    std::string_view line = text.substr(lineStart, cursor - lineStart);
    std::size_t offset = 0;
    while (offset < line.size() and (line[offset] == ' ' or line[offset] == '\t'))
        ++offset;
    constexpr std::string_view Directive = "#تضمين";
    if (not line.substr(offset).starts_with(Directive)) return std::nullopt;
    offset += Directive.size();
    while (offset < line.size() and (line[offset] == ' ' or line[offset] == '\t'))
        ++offset;
    if (offset >= line.size() or line[offset] != '"') return std::nullopt;
    ++offset;
    if (line.substr(offset).find('"') != std::string_view::npos) return std::nullopt;

    const std::string typed(line.substr(offset));
    const std::size_t separator = typed.find_last_of("/\\");
    const std::string directoryPrefix = separator == std::string::npos
        ? std::string{} : typed.substr(0, separator + 1);
    const std::string leafPrefix = separator == std::string::npos
        ? typed : typed.substr(separator + 1);
    const std::size_t replacementStart = lineStart + offset +
        (separator == std::string::npos ? 0 : separator + 1);
    const Json replacementRange{
        {"start", PositionEncoding::utf16PositionForByteOffset(text, replacementStart)},
        {"end", PositionEncoding::utf16PositionForByteOffset(text, cursor)}
    };

    Json items = Json::array();
    std::unordered_set<std::string> seen;
    for (const std::filesystem::path &root : searchRoots) {
        std::filesystem::path directory = root;
        if (not directoryPrefix.empty())
            directory /= pathFromUtf8(directoryPrefix);
        std::error_code error;
        if (not std::filesystem::is_directory(directory, error) or error) continue;
        std::filesystem::directory_iterator iterator(directory, error);
        const std::filesystem::directory_iterator end;
        for (; not error and iterator != end; iterator.increment(error)) {
            if (not isIncludeCandidate(*iterator)) continue;
            const std::string name = utf8FromPath(iterator->path().filename());
            if (not leafPrefix.empty() and not name.starts_with(leafPrefix)) continue;
            std::error_code typeError;
            const bool directoryEntry = iterator->is_directory(typeError) and not typeError;
            const std::string insertion = name + (directoryEntry ? "/" : "");
            if (not seen.insert(insertion).second) continue;
            items.push_back({
                {"label", insertion},
                {"kind", directoryEntry ? 19 : 17},
                {"detail", directoryEntry
                    ? "افتح المجلد لعرض ملفات التضمين"
                    : "ملف يمكن تضمينه في المصدر"},
                {"filterText", name},
                {"insertTextFormat", 1},
                {"sortText", std::string(directoryEntry ? "0" : "1") + name},
                {"textEdit", {{"range", replacementRange}, {"newText", insertion}}},
                {"data", {{"source", "baa-lsp-include"}}}
            });
        }
    }
    std::sort(items.begin(), items.end(), [](const Json &left, const Json &right) {
        return left.value("sortText", "") < right.value("sortText", "");
    });
    return items;
}

void appendWorkspaceSymbols(Json &output,
                            std::string_view uri,
                            std::string_view text,
                            const Json &symbols,
                            std::string_view container)
{
    if (not symbols.is_array()) return;
    for (const Json &symbol : symbols) {
        if (not symbol.is_object()) continue;
        const std::string name = symbol.value("name", "");
        const std::string baaKind = symbol.value("kind", "");
        if (name.empty()) continue;

        const Json converted = PositionEncoding::baaSymbolsToLsp(
            text, Json::array({symbol}));
        if (baaKind != "parameter" and converted.is_array() and
            not converted.empty() and converted.front().is_object()) {
            const Json &documentSymbol = converted.front();
            const Json range = documentSymbol.value(
                "selectionRange", Json::object());
            if (range.is_object() and range.contains("start") and
                range.contains("end")) {
                Json data{{"baaKind", baaKind}};
                const std::string detail =
                    documentSymbol.value("detail", "");
                if (not detail.empty()) data["detail"] = detail;
                Json item{
                    {"name", name},
                    {"kind", documentSymbol.value("kind", 13)},
                    {"location", {
                        {"uri", std::string(uri)},
                        {"range", range}
                    }},
                    {"data", std::move(data)}
                };
                if (not container.empty())
                    item["containerName"] = std::string(container);
                output.push_back(std::move(item));
            }
        }

        appendWorkspaceSymbols(
            output,
            uri,
            text,
            symbol.value("children", Json::array()),
            name);
    }
}

bool workspaceSymbolMatches(std::string_view name, std::string_view query)
{
    return query.empty() or name.find(query) != std::string_view::npos;
}
}

BaaLanguageServer::BaaLanguageServer()
{
    m_compiler.setAnalysisCallback(
        [this](BaaAnalysisResult result) { onAnalysisFinished(std::move(result)); });
    m_compiler.setSymbolCallback(
        [this](BaaSymbolResult result) { onSymbolsFinished(std::move(result)); });
    m_compiler.setTokenCallback(
        [this](BaaTokenResult result) { onTokensFinished(std::move(result)); });
    m_compiler.setStructureCallback(
        [this](BaaStructureResult result) {
            onStructureFinished(std::move(result));
        });
    m_compiler.setInlayHintCallback(
        [this](BaaInlayHintResult result) {
            onInlayHintsFinished(std::move(result));
        });
    m_compiler.setCompletionDataCallback(
        [this](BaaCompletionDataResult result) {
            onCompletionDataFinished(std::move(result));
        });
    m_compiler.setFormatCallback(
        [this](BaaFormatResult result) { onFormatFinished(std::move(result)); });
    m_compiler.setSemanticCallback(
        [this](BaaSemanticResult result) { onSemanticFinished(std::move(result)); });
}

BaaLanguageServer::~BaaLanguageServer()
{
    m_compiler.setAnalysisCallback({});
    m_compiler.setSymbolCallback({});
    m_compiler.setTokenCallback({});
    m_compiler.setStructureCallback({});
    m_compiler.setInlayHintCallback({});
    m_compiler.setCompletionDataCallback({});
    m_compiler.setFormatCallback({});
    m_compiler.setSemanticCallback({});
    m_compiler.cancelAll();
}

void BaaLanguageServer::setCompilerProgram(std::string program)
{
    m_compilerProgram = program.empty()
        ? std::filesystem::path{}
        : pathFromUtf8(program);
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

void BaaLanguageServer::initializeWorkspace(const Json &initializeParams)
{
    m_initializationOptions = objectValue(
        initializeParams, "initializationOptions");
    m_workspaceRoots.clear();
    m_projectPlans.clear();
    invalidateProjectContext(
        "Project context changed while the language server initialized.");

    bool addedFolder = false;
    const auto folders = initializeParams.find("workspaceFolders");
    if (folders != initializeParams.end() and folders->is_array()) {
        for (const Json &folder : *folders) {
            if (not folder.is_object()) continue;
            const std::string pathText = localPathForUri(
                stringValue(folder, "uri"));
            if (pathText.empty()) continue;
            addedFolder = addWorkspaceRoot(pathFromUtf8(pathText)) or
                addedFolder;
        }
    }
    if (addedFolder) return;

    const std::string rootPathText = localPathForUri(
        stringValue(initializeParams, "rootUri"));
    if (not rootPathText.empty())
        addWorkspaceRoot(pathFromUtf8(rootPathText));
}

bool BaaLanguageServer::addWorkspaceRoot(const std::filesystem::path &root)
{
    std::filesystem::path normalized;
    try {
        normalized = std::filesystem::absolute(root).lexically_normal();
    } catch (const std::filesystem::filesystem_error &) {
        sendLogEvent("workspace.root.invalid", "workspace",
                     "Workspace folder could not be normalized.", 2);
        return false;
    }
    const std::string key = comparablePath(normalized);
    if (key.empty() or m_workspaceRoots.contains(key)) return false;
    m_workspaceRoots.emplace(key, normalized);
    if (not loadProjectPlan(normalized, false)) {
        invalidateProjectContext(
            "Workspace folders changed before project data was ready.");
    }
    return true;
}

bool BaaLanguageServer::removeWorkspaceRoot(
    const std::filesystem::path &root)
{
    const std::string key = comparablePath(root);
    if (key.empty() or m_workspaceRoots.erase(key) == 0) return false;
    m_projectPlans.erase(key);
    invalidateProjectContext(
        "Workspace folders changed before project data was ready.");
    sendLogEvent("workspace.root.removed", "workspace",
                 "A workspace folder was removed.", 3);
    return true;
}

bool BaaLanguageServer::loadProjectPlan(const std::filesystem::path &root,
                                        bool reportMissingManifest)
{
    const std::string key = comparablePath(root);
    if (key.empty()) return false;
    const std::filesystem::path manifest =
        root / pathFromUtf8("مشروع.تكوين");
    std::error_code filesystemError;
    if (not std::filesystem::is_regular_file(manifest, filesystemError)) {
        const bool removed = m_projectPlans.erase(key) != 0;
        if (reportMissingManifest) {
            sendLogEvent(
                "workspace.manifest.missing", "workspace",
                "Takween project context was removed because مشروع.تكوين "
                "is not present.", 2);
        }
        if (removed) {
            invalidateProjectContext(
                "Takween project context changed before semantic data was ready.");
        }
        return false;
    }

    const Json &options = m_initializationOptions;
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
        const bool removed = m_projectPlans.erase(key) != 0;
        sendLogEvent(
            "workspace.plan.unavailable", "workspace",
            "Takween is unavailable while loading the project plan.", 2);
        if (removed) {
            invalidateProjectContext(
                "Takween project context failed to reload.");
        }
        return false;
    }
    if (process.exitCode != 0) {
        const bool removed = m_projectPlans.erase(key) != 0;
        sendLogEvent(
            "workspace.plan.failed", "workspace",
            "Takween failed while loading the project plan.", 2,
            {{"exit_code", process.exitCode}});
        if (removed) {
            invalidateProjectContext(
                "Takween project context failed to reload.");
        }
        return false;
    }

    const Json plan = Json::parse(process.standardOutput, nullptr, false);
    if (plan.is_discarded() or not plan.is_object() or
        plan.value("schema_version", "") != "takween-build-plan-v1" or
        not plan.value("source_files", Json::array()).is_array() or
        not plan.value("include_paths", Json::array()).is_array()) {
        const bool removed = m_projectPlans.erase(key) != 0;
        sendLogEvent(
            "workspace.plan.invalid", "workspace",
            "Takween returned a project plan that does not satisfy "
            "takween-build-plan-v1.", 2);
        if (removed) {
            invalidateProjectContext(
                "Takween project context failed to reload.");
        }
        return false;
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
        if (not isBaaImplementationPath(path)) continue;
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
    if (not loaded.loaded) {
        const bool removed = m_projectPlans.erase(key) != 0;
        sendLogEvent(
            "workspace.plan.empty", "workspace",
            "Takween returned a project plan without Baa translation units.",
            2, {{"source_count", 0}});
        if (removed) {
            invalidateProjectContext(
                "Takween project context failed to reload.");
        }
        return false;
    }
    const std::size_t sourceCount = loaded.sourceFiles.size();
    m_projectPlans.insert_or_assign(key, std::move(loaded));
    invalidateProjectContext(
        "Takween project context changed before semantic data was ready.");
    if (m_projectPlans.at(key).loaded) {
        sendLogEvent(
            "workspace.plan.loaded", "workspace",
            "Loaded the Takween project plan.", 3,
            {{"source_count", sourceCount}});
    }
    return true;
}

void BaaLanguageServer::invalidateProjectContext(const std::string &message)
{
    invalidateWorkspaceSymbolRequests(ContentModified, message);
    invalidateSymbolRequests({}, ContentModified, message);
    invalidateTokenRequests({}, ContentModified, message);
    invalidateInlayHintRequests({}, ContentModified, message);
    invalidateSemanticRequests({}, ContentModified, message);
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        m_symbolCache.clear();
        m_workspaceSymbolIndex.clear();
    }
    {
        std::scoped_lock lock(m_tokenRequestsMutex);
        m_tokenCache.clear();
    }
    {
        std::scoped_lock lock(m_inlayHintMutex);
        m_inlayHintCache.clear();
    }
    {
        std::scoped_lock lock(m_semanticMutex);
        m_semanticCache.clear();
    }
}

const BaaLanguageServer::ProjectPlan *BaaLanguageServer::projectPlanForPath(
    const std::filesystem::path &path) const
{
    const ProjectPlan *selected = nullptr;
    std::size_t selectedRootLength = 0;
    for (const auto &[key, plan] : m_projectPlans) {
        (void)key;
        if (not plan.loaded or not pathIsWithin(path, plan.root)) continue;
        const std::size_t rootLength = comparablePath(plan.root).size();
        if (not selected or rootLength > selectedRootLength) {
            selected = &plan;
            selectedRootLength = rootLength;
        }
    }
    return selected;
}

bool BaaLanguageServer::hasLoadedProjectPlan() const
{
    return std::ranges::any_of(
        m_projectPlans,
        [](const auto &entry) { return entry.second.loaded; });
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
        const Json initializationOptions = objectValue(
            initializeParams, "initializationOptions");
        const Json structuredLogs = objectValue(
            initializationOptions, "baaStructuredLogs");
        {
            std::scoped_lock lock(m_logMutex);
            m_structuredLogsEnabled =
                stringValue(structuredLogs, "schemaVersion") ==
                StructuredLogSchema;
            m_nextLogSequence = 1;
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
                {"foldingRangeProvider", true},
                {"selectionRangeProvider", true},
                {"inlayHintProvider", true},
                {"semanticTokensProvider", {
                    {"legend", {
                        {"tokenTypes", Json::array({
                            "type",
                            "macro",
                            "keyword",
                            "modifier",
                            "comment",
                            "string",
                            "number",
                            "operator",
                            "function",
                            "variable",
                            "parameter",
                            "property",
                            "enumMember"
                        })},
                        {"tokenModifiers", Json::array()}
                    }},
                    {"full", true}
                }},
                {"workspaceSymbolProvider", true},
                {"workspace", {
                    {"workspaceFolders", {
                        {"supported", true},
                        {"changeNotifications", true}
                    }}
                }},
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"referencesProvider", true},
                {"documentFormattingProvider", true},
                {"codeActionProvider", {
                    {"codeActionKinds", Json::array({"quickfix"})},
                    {"resolveProvider", false}
                }},
                {"renameProvider", {{"prepareProvider", true}}},
                {"signatureHelpProvider", {
                    {"triggerCharacters", Json::array({"(", "،", ","})},
                    {"retriggerCharacters", Json::array({"،", ","})}
                }},
                {"completionProvider", {
                    {"resolveProvider", true},
                    {"triggerCharacters", completionTriggerCharacters()}
                }},
                {"experimental", {
                    {"baaLogEvent", {
                        {"schemaVersion", std::string(StructuredLogSchema)},
                        {"transport", "local-stdio"},
                        {"telemetry", false}
                    }}
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
        initializeWorkspace(initializeParams);
        return;
    }
    if (not m_initializeResponded) {
        sendError(id, ServerNotInitialized, "Language server is not initialized.");
        return;
    }
    if (method == "shutdown") {
        m_shutdownRequested = true;
        invalidateSymbolRequests({}, RequestCancelled, "Request cancelled during shutdown.");
        invalidateTokenRequests({}, RequestCancelled,
                                "Semantic tokens request cancelled during shutdown.");
        invalidateStructureRequests(
            {}, RequestCancelled,
            "Structural editing request cancelled during shutdown.");
        invalidateInlayHintRequests(
            {}, RequestCancelled,
            "Inlay hint request cancelled during shutdown.");
        invalidateWorkspaceSymbolRequests(
            RequestCancelled, "Workspace symbol request cancelled during shutdown.");
        invalidateCompletionRequests({}, RequestCancelled,
                                     "Request cancelled during shutdown.");
        invalidateFormatRequests({}, RequestCancelled,
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
    if (method == "textDocument/semanticTokens/full") {
        handleSemanticTokensFull(id, objectValue(message, "params"));
        return;
    }
    if (method == "textDocument/foldingRange") {
        handleFoldingRange(id, objectValue(message, "params"));
        return;
    }
    if (method == "textDocument/selectionRange") {
        handleSelectionRange(id, objectValue(message, "params"));
        return;
    }
    if (method == "textDocument/inlayHint") {
        handleInlayHint(id, objectValue(message, "params"));
        return;
    }
    if (method == "workspace/symbol") {
        handleWorkspaceSymbol(id, objectValue(message, "params"));
        return;
    }
    if (method == "textDocument/completion") {
        handleCompletion(id, objectValue(message, "params"));
        return;
    }
    if (method == "completionItem/resolve") {
        handleCompletionResolve(id, objectValue(message, "params"));
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
    if (method == "textDocument/codeAction") {
        handleCodeAction(id, objectValue(message, "params"));
        return;
    }
    if (method == "textDocument/formatting") {
        handleDocumentFormatting(id, objectValue(message, "params"));
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

    if (method == "workspace/didChangeWorkspaceFolders")
        handleDidChangeWorkspaceFolders(params);
    else if (method == "workspace/didChangeWatchedFiles")
        handleDidChangeWatchedFiles(params);
    else if (method == "textDocument/didOpen") handleDidOpen(params);
    else if (method == "textDocument/didChange") handleDidChange(params);
    else if (method == "textDocument/didSave") handleDidSave(params);
    else if (method == "textDocument/didClose") handleDidClose(params);
}

void BaaLanguageServer::handleDidChangeWorkspaceFolders(const Json &params)
{
    const Json event = objectValue(params, "event");
    const auto removed = event.find("removed");
    if (removed != event.end() and removed->is_array()) {
        for (const Json &folder : *removed) {
            if (not folder.is_object()) continue;
            const std::string path = localPathForUri(stringValue(folder, "uri"));
            if (not path.empty()) removeWorkspaceRoot(pathFromUtf8(path));
        }
    }
    const auto added = event.find("added");
    if (added != event.end() and added->is_array()) {
        for (const Json &folder : *added) {
            if (not folder.is_object()) continue;
            const std::string path = localPathForUri(stringValue(folder, "uri"));
            if (not path.empty()) addWorkspaceRoot(pathFromUtf8(path));
        }
    }
}

void BaaLanguageServer::handleDidChangeWatchedFiles(const Json &params)
{
    const auto changes = params.find("changes");
    if (changes == params.end() or not changes->is_array()) return;

    std::unordered_set<std::string> rootsToReload;
    for (const Json &change : *changes) {
        if (not change.is_object()) continue;
        const std::string pathText = localPathForUri(stringValue(change, "uri"));
        if (pathText.empty()) continue;
        const std::filesystem::path changed = pathFromUtf8(pathText);
        const std::filesystem::path name = changed.filename();
        if (name != pathFromUtf8("مشروع.تكوين") and
            name != pathFromUtf8("تكوين.قفل"))
            continue;
        const std::string rootKey = comparablePath(changed.parent_path());
        if (m_workspaceRoots.contains(rootKey)) rootsToReload.insert(rootKey);
    }

    for (const std::string &rootKey : rootsToReload) {
        const auto root = m_workspaceRoots.find(rootKey);
        if (root != m_workspaceRoots.end())
            loadProjectPlan(root->second, true);
    }
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
            sendLogEvent("document.open.rejected", "document",
                         "A Baa document was rejected while opening.");
            return;
        }
    }
    invalidateSymbolRequests(
        document.uri,
        ContentModified,
        "Document opened before workspace symbols were ready.");
    invalidateTokenRequests(
        document.uri,
        ContentModified,
        "Document opened before semantic tokens were ready.");
    invalidateStructureRequests(
        document.uri,
        ContentModified,
        "Document opened before structural editing data was ready.");
    invalidateInlayHintRequests(
        document.uri,
        ContentModified,
        "Document opened before inlay hints were ready.");
    {
        std::scoped_lock lock(m_tokenRequestsMutex);
        m_tokenCache.erase(document.uri);
    }
    {
        std::scoped_lock lock(m_structureMutex);
        m_structureCache.erase(document.uri);
    }
    {
        std::scoped_lock lock(m_inlayHintMutex);
        m_inlayHintCache.erase(document.uri);
    }
    invalidateWorkspaceSymbolRequests(
        ContentModified,
        "Workspace symbols became obsolete after a document opened.");
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        m_workspaceSymbolIndex.erase(document.uri);
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
        sendLogEvent("document.change.invalid", "document",
                     "Document change does not contain valid full text.");
        return;
    }

    std::string error;
    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.change(uri, version, changes->front()["text"].get<std::string>(),
                                   &error)) {
            sendLogEvent("document.change.rejected", "document",
                         "A Baa document change was rejected.", 2);
            return;
        }
        document = m_documents.document(uri);
    }
    invalidateSymbolRequests(uri, ContentModified,
                             "Document changed before symbols were ready.");
    invalidateTokenRequests(uri, ContentModified,
                            "Document changed before semantic tokens were ready.");
    invalidateStructureRequests(
        uri, ContentModified,
        "Document changed before structural editing data was ready.");
    invalidateInlayHintRequests(
        uri, ContentModified,
        "Document changed before inlay hints were ready.");
    invalidateWorkspaceSymbolRequests(
        ContentModified,
        "Workspace symbols became obsolete after a document change.");
    invalidateCompletionRequests(uri, ContentModified,
                                 "Document changed before completion was ready.");
    invalidateFormatRequests(uri, ContentModified,
                             "Document changed before formatting was ready.");
    invalidateSemanticRequests(uri, ContentModified,
                               "Document changed before semantic data was ready.");
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        m_symbolCache.erase(uri);
        m_workspaceSymbolIndex.erase(uri);
    }
    {
        std::scoped_lock lock(m_tokenRequestsMutex);
        m_tokenCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_structureMutex);
        m_structureCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_inlayHintMutex);
        m_inlayHintCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_semanticMutex);
        if (hasLoadedProjectPlan()) m_semanticCache.clear();
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
    invalidateTokenRequests(uri, ContentModified,
                            "Document closed before semantic tokens were ready.");
    invalidateStructureRequests(
        uri, ContentModified,
        "Document closed before structural editing data was ready.");
    invalidateInlayHintRequests(
        uri, ContentModified,
        "Document closed before inlay hints were ready.");
    invalidateWorkspaceSymbolRequests(
        ContentModified,
        "Workspace symbols became obsolete after a document closed.");
    invalidateCompletionRequests(uri, ContentModified,
                                 "Document closed before completion was ready.");
    invalidateFormatRequests(uri, ContentModified,
                             "Document closed before formatting was ready.");
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
        m_workspaceSymbolIndex.erase(uri);
    }
    {
        std::scoped_lock lock(m_tokenRequestsMutex);
        m_tokenCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_structureMutex);
        m_structureCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_inlayHintMutex);
        m_inlayHintCache.erase(uri);
    }
    {
        std::scoped_lock lock(m_semanticMutex);
        if (hasLoadedProjectPlan()) m_semanticCache.clear();
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

void BaaLanguageServer::handleSemanticTokensFull(const Json &id,
                                                 const Json &params)
{
    const std::string uri =
        stringValue(objectValue(params, "textDocument"), "uri");
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
        std::scoped_lock lock(m_tokenRequestsMutex);
        const auto it = m_tokenCache.find(uri);
        if (it != m_tokenCache.end() and
            it->second.version == document.version) {
            cached = it->second.tokens;
            cachedText = it->second.text;
        }
    }
    if (cached.is_array()) {
        sendResult(
            id,
            {{"data", PositionEncoding::baaTokensToLspData(
                cachedText, cached)}});
        return;
    }

    const std::string path = localPathForUri(uri);
    if (path.empty()) {
        sendError(id, InvalidParams,
                  "Baa-LSP supports local file:// documents only.");
        return;
    }

    std::uint64_t token{};
    {
        std::scoped_lock lock(m_tokenRequestsMutex);
        for (auto &[existingToken, pending] : m_tokenRequests) {
            if (pending.uri == uri and pending.version == document.version) {
                pending.ids.push_back(id);
                return;
            }
        }
        token = m_nextTokenToken++;
        if (token == 0) token = m_nextTokenToken++;
        m_tokenRequests.emplace(
            token,
            PendingTokenRequest{{id}, uri, document.version});
    }

    BaaTokenRequest request;
    request.token = token;
    request.uri = uri;
    request.filePath = path;
    request.text = document.text;
    request.version = document.version;
    if (const ProjectPlan *plan = projectPlanForPath(pathFromUtf8(path))) {
        request.projectWorkingDirectory = plan->workingDirectory;
        request.includePaths = plan->includePaths;
    }
    m_compiler.requestTokens(std::move(request));
}

void BaaLanguageServer::handleFoldingRange(const Json &id,
                                           const Json &params)
{
    handleStructureRequest(id, params, StructureReplyKind::Folding);
}

void BaaLanguageServer::handleSelectionRange(const Json &id,
                                             const Json &params)
{
    handleStructureRequest(id, params, StructureReplyKind::Selection);
}

void BaaLanguageServer::handleStructureRequest(const Json &id,
                                               const Json &params,
                                               StructureReplyKind kind)
{
    const std::string uri =
        stringValue(objectValue(params, "textDocument"), "uri");
    if (uri.empty()) {
        sendError(id, InvalidParams, "textDocument.uri is required.");
        return;
    }

    Json positions = Json::array();
    if (kind == StructureReplyKind::Selection) {
        const auto requested = params.find("positions");
        if (requested == params.end() or not requested->is_array() or
            requested->empty()) {
            sendError(id, InvalidParams,
                      "selectionRange requires a non-empty positions array.");
            return;
        }
        for (const Json &position : *requested) {
            int line{};
            int character{};
            if (not position.is_object() or
                not nonnegativeIntValue(
                    position.value("line", Json(nullptr)), &line) or
                not nonnegativeIntValue(
                    position.value("character", Json(nullptr)), &character)) {
                sendError(id, InvalidParams,
                          "selectionRange positions must be non-negative integers.");
                return;
            }
            positions.push_back({{"line", line}, {"character", character}});
        }
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

    CachedStructure cached;
    bool hasCached = false;
    {
        std::scoped_lock lock(m_structureMutex);
        const auto existing = m_structureCache.find(uri);
        if (existing != m_structureCache.end() and
            existing->second.version == document.version) {
            cached = existing->second;
            hasCached = true;
        }
    }
    if (hasCached) {
        if (kind == StructureReplyKind::Folding) {
            sendResult(id, PositionEncoding::baaFoldingRangesToLsp(
                cached.text, cached.foldingRanges));
        } else {
            sendResult(id, PositionEncoding::baaSelectionRangesToLsp(
                cached.text, cached.selectionRanges, positions));
        }
        return;
    }

    const std::string path = localPathForUri(uri);
    if (path.empty()) {
        sendError(id, InvalidParams,
                  "Baa-LSP supports local file:// documents only.");
        return;
    }

    std::uint64_t token{};
    {
        std::scoped_lock lock(m_structureMutex);
        for (auto &[existingToken, pending] : m_structureRequests) {
            if (pending.uri == uri and pending.version == document.version) {
                pending.replies.push_back({id, kind, std::move(positions)});
                return;
            }
        }
        token = m_nextStructureToken++;
        if (token == 0) token = m_nextStructureToken++;
        m_structureRequests.emplace(
            token,
            PendingStructureRequest{
                {{id, kind, std::move(positions)}}, uri, document.version
            });
    }

    m_compiler.requestStructure({
        token, uri, path, document.text, document.version
    });
}

void BaaLanguageServer::handleInlayHint(const Json &id, const Json &params)
{
    const std::string uri =
        stringValue(objectValue(params, "textDocument"), "uri");
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
    const auto range = lspByteRange(
        document.text, params.value("range", Json(nullptr)));
    if (not range) {
        sendError(id, InvalidParams,
                  "inlayHint requires a valid UTF-16 document range.");
        return;
    }

    CachedInlayHints cached;
    bool hasCached = false;
    {
        std::scoped_lock lock(m_inlayHintMutex);
        const auto existing = m_inlayHintCache.find(uri);
        if (existing != m_inlayHintCache.end() and
            existing->second.version == document.version) {
            cached = existing->second;
            hasCached = true;
        }
    }
    if (hasCached) {
        sendResult(id, inlayHintsForRange(
            cached.text, cached.hints, range->first, range->second,
            cached.complete));
        return;
    }

    const std::string path = localPathForUri(uri);
    if (path.empty()) {
        sendError(id, InvalidParams,
                  "Baa-LSP supports local file:// documents only.");
        return;
    }

    std::uint64_t token{};
    {
        std::scoped_lock lock(m_inlayHintMutex);
        for (auto &[existingToken, pending] : m_inlayHintRequests) {
            (void)existingToken;
            if (pending.uri == uri and pending.version == document.version) {
                pending.replies.push_back(
                    {id, range->first, range->second});
                return;
            }
        }
        token = m_nextInlayHintToken++;
        if (token == 0) token = m_nextInlayHintToken++;
        m_inlayHintRequests.emplace(
            token,
            PendingInlayHintRequest{
                {{id, range->first, range->second}}, uri, document.version
            });
    }

    BaaInlayHintRequest request;
    request.token = token;
    request.uri = uri;
    request.filePath = path;
    request.text = document.text;
    request.version = document.version;
    if (const ProjectPlan *plan = projectPlanForPath(pathFromUtf8(path))) {
        request.projectWorkingDirectory = plan->workingDirectory;
        request.includePaths = plan->includePaths;
    }
    m_compiler.requestInlayHints(std::move(request));
}

void BaaLanguageServer::handleWorkspaceSymbol(const Json &id,
                                              const Json &params)
{
    const auto query = params.find("query");
    if (query == params.end() or not query->is_string()) {
        sendError(id, InvalidParams, "workspace/symbol requires a string query.");
        return;
    }

    struct WorkspaceSource
    {
        std::string uri;
        std::string filePath;
        std::string text;
        int version{};
        bool openDocument{};
    };

    std::vector<BaaDocument> openDocuments;
    {
        std::scoped_lock lock(m_documentsMutex);
        openDocuments = m_documents.documents();
    }
    std::unordered_map<std::string, BaaDocument> openByPath;
    for (const BaaDocument &document : openDocuments) {
        const std::string path = localPathForUri(document.uri);
        if (path.empty()) continue;
        const std::string comparable = comparablePath(pathFromUtf8(path));
        if (not comparable.empty())
            openByPath.insert_or_assign(comparable, document);
    }

    std::vector<WorkspaceSource> sources;
    std::unordered_set<std::string> seenUris;
    auto appendSource = [&](const std::filesystem::path &sourcePath) -> bool {
        const std::string path = utf8FromPath(sourcePath);
        const std::string comparable = comparablePath(sourcePath);
        const auto open = openByPath.find(comparable);
        WorkspaceSource source;
        source.filePath = path;
        if (open != openByPath.end()) {
            source.uri = open->second.uri;
            source.text = open->second.text;
            source.version = open->second.version;
            source.openDocument = true;
        } else {
            source.uri = fileUriForPath(sourcePath);
            const std::optional<std::string> text = readUtf8File(sourcePath);
            if (source.uri.empty() or not text) return false;
            source.text = *text;
        }
        if (seenUris.insert(source.uri).second)
            sources.push_back(std::move(source));
        return true;
    };

    for (const auto &[root, plan] : m_projectPlans) {
        (void)root;
        if (not plan.loaded) continue;
        for (const std::filesystem::path &source : plan.sourceFiles) {
            if (not appendSource(source)) {
                sendError(
                    id,
                    InternalError,
                    "A Takween project source could not be read for workspace symbols.");
                return;
            }
        }
    }
    for (const BaaDocument &document : openDocuments) {
        const std::string pathText = localPathForUri(document.uri);
        if (pathText.empty()) continue;
        const std::filesystem::path path = pathFromUtf8(pathText);
        if (not isBaaDocumentPath(path))
            continue;
        if (seenUris.insert(document.uri).second) {
            sources.push_back({
                document.uri,
                pathText,
                document.text,
                document.version,
                true
            });
        }
    }

    if (sources.empty()) {
        sendResult(id, Json::array());
        return;
    }

    PendingWorkspaceSymbolRequest pending;
    pending.id = id;
    pending.query = query->get<std::string>();
    std::vector<WorkspaceSource> missing;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        for (const WorkspaceSource &source : sources) {
            pending.sources.push_back(
                {source.uri, source.version, source.openDocument});
            const auto indexed = m_workspaceSymbolIndex.find(source.uri);
            if (indexed == m_workspaceSymbolIndex.end() or
                indexed->second.version != source.version or
                indexed->second.openDocument != source.openDocument) {
                missing.push_back(source);
            }
        }
        if (not missing.empty())
            m_pendingWorkspaceSymbolRequests.push_back(std::move(pending));
    }

    if (missing.empty()) {
        sendResult(id, workspaceSymbolResult(query->get<std::string>()));
        return;
    }
    for (WorkspaceSource &source : missing) {
        requestSymbols(
            std::move(source.uri),
            std::move(source.filePath),
            std::move(source.text),
            source.version,
            source.openDocument,
            true);
    }
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
    requestSymbols(
        document.uri,
        path,
        document.text,
        document.version,
        true,
        false,
        requestId);
}

void BaaLanguageServer::requestSymbols(std::string uri,
                                       std::string filePath,
                                       std::string text,
                                       int version,
                                       bool requireOpenDocument,
                                       bool workspaceIndex,
                                       const Json *requestId)
{
    std::uint64_t token{};
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        for (auto &[existingToken, pending] : m_symbolRequests) {
            if (pending.uri == uri and pending.version == version and
                pending.requireOpenDocument == requireOpenDocument) {
                if (requestId) pending.ids.push_back(*requestId);
                pending.workspaceIndex =
                    pending.workspaceIndex or workspaceIndex;
                return;
            }
        }
        token = m_nextSymbolToken++;
        if (token == 0) token = m_nextSymbolToken++;
        PendingSymbolRequest pending;
        pending.uri = uri;
        pending.version = version;
        pending.requireOpenDocument = requireOpenDocument;
        pending.workspaceIndex = workspaceIndex;
        if (requestId) pending.ids.push_back(*requestId);
        m_symbolRequests.emplace(token, std::move(pending));
    }
    BaaSymbolRequest request;
    request.token = token;
    request.uri = std::move(uri);
    const ProjectPlan *plan = projectPlanForPath(pathFromUtf8(filePath));
    request.filePath = std::move(filePath);
    request.text = std::move(text);
    request.version = version;
    if (plan) request.includePaths = plan->includePaths;
    request.requireLatestVersion = requireOpenDocument;
    m_compiler.requestSymbols(std::move(request));
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

    BaaDocument completionDocument;
    {
        std::scoped_lock lock(m_documentsMutex);
        completionDocument = m_documents.document(uri);
    }
    const std::string documentPathText = localPathForUri(uri);
    const std::filesystem::path documentPath = pathFromUtf8(documentPathText);
    std::vector<std::filesystem::path> includeRoots;
    includeRoots.push_back(documentPath.parent_path());
    if (const ProjectPlan *plan = projectPlanForPath(documentPath)) {
        includeRoots.push_back(plan->workingDirectory);
        for (const std::string &path : plan->includePaths)
            includeRoots.push_back(pathFromUtf8(path));
    }
    if (const char *stdlib = std::getenv("BAA_STDLIB"); stdlib and *stdlib)
        includeRoots.push_back(pathFromUtf8(stdlib));
    if (const char *home = std::getenv("BAA_HOME"); home and *home)
        includeRoots.push_back(pathFromUtf8(home) / "stdlib");
    // Qalam passes the resolved Baa executable explicitly. Discovering the
    // sibling stdlib from that path keeps completion reliable even when Qalam
    // was already running before an installer broadcast new environment
    // variables to Windows applications.
    if (not m_compilerProgram.empty() and
        not m_compilerProgram.parent_path().empty()) {
        includeRoots.push_back(m_compilerProgram.parent_path() / "stdlib");
    }
    const std::size_t cursor =
        PositionEncoding::utf8ByteOffsetForUtf16Position(
            completionDocument.text, line, character);
    if (const std::optional<Json> includeItems = buildIncludePathCompletion(
            completionDocument.text, cursor, includeRoots)) {
        sendResult(id, {{"isIncomplete", false}, {"items", *includeItems}});
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

    handleSemanticRequest(
        id,
        {
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}}
        },
        SemanticReplyKind::Completion);
}

void BaaLanguageServer::handleCompletionResolve(const Json &id,
                                                const Json &item)
{
    if (not item.is_object()) {
        sendError(id, InvalidParams,
                  "completionItem/resolve requires a completion item.");
        return;
    }
    Json resolved = item;
    const Json data = objectValue(item, "data");
    const std::string documentation =
        stringValue(data, "documentation");
    if (not documentation.empty()) {
        resolved["documentation"] = {
            {"kind", "markdown"},
            {"value", documentation}
        };
    }
    sendResult(id, resolved);
}

void BaaLanguageServer::handleCodeAction(const Json &id, const Json &params)
{
    const std::string uri = stringValue(objectValue(params, "textDocument"), "uri");
    const Json requestedRange = objectValue(params, "range");
    if (uri.empty() or not requestedRange.contains("start") or
        not requestedRange.contains("end")) {
        sendError(id, InvalidParams,
                  "textDocument.uri and a UTF-16 range are required.");
        return;
    }

    const Json only = objectValue(params, "context").value(
        "only", Json::array());
    if (only.is_array() and not only.empty()) {
        bool acceptsQuickFix = false;
        for (const Json &kind : only) {
            if (kind.is_string() and kind.get<std::string>() == "quickfix") {
                acceptsQuickFix = true;
                break;
            }
        }
        if (not acceptsQuickFix) {
            sendResult(id, Json::array());
            return;
        }
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
    const auto selection = lspByteRange(document.text, requestedRange);
    if (not selection) {
        sendError(id, InvalidParams,
                  "Code-action range is outside the current UTF-16 document.");
        return;
    }

    PublishedDiagnostics published;
    {
        std::scoped_lock lock(m_diagnosticsMutex);
        const auto it = m_publishedDiagnostics.find(uri);
        if (it == m_publishedDiagnostics.end() or
            it->second.version != document.version) {
            sendResult(id, Json::array());
            return;
        }
        published = it->second;
    }

    Json matching = Json::array();
    for (const Json &diagnostic : published.diagnostics) {
        if (not diagnostic.is_object()) continue;
        const Json data = objectValue(diagnostic, "data");
        const Json fixes = data.value("fixes", Json::array());
        if (not fixes.is_array() or fixes.empty()) continue;
        const auto diagnosticRange =
            lspByteRange(document.text, objectValue(diagnostic, "range"));
        if (diagnosticRange and lspRangesIntersect(*selection, *diagnosticRange))
            matching.push_back(diagnostic);
    }

    const std::string currentPath = localPathForUri(uri);
    const std::string currentComparable = currentPath.empty()
        ? std::string{}
        : comparablePath(pathFromUtf8(currentPath));
    Json actions = Json::array();
    std::unordered_set<std::string> seen;
    for (const Json &diagnostic : matching) {
        const Json fixes = objectValue(diagnostic, "data").value(
            "fixes", Json::array());
        for (const Json &fix : fixes) {
            if (not fix.is_object() or
                stringValue(fix, "kind") != "quickfix" or
                stringValue(fix, "applicability") != "safe") continue;
            const std::string title = stringValue(fix, "title");
            const Json rawEdits = fix.value("edits", Json::array());
            if (title.empty() or not rawEdits.is_array() or rawEdits.empty())
                continue;

            Json edits = Json::array();
            std::vector<std::size_t> insertionOffsets;
            bool valid = not currentComparable.empty();
            for (const Json &rawEdit : rawEdits) {
                const std::string editFile = stringValue(rawEdit, "file");
                std::filesystem::path editPath = pathFromUtf8(editFile);
                if (editPath.is_relative()) {
                    editPath =
                        pathFromUtf8(currentPath).parent_path() / editPath;
                }
                const auto editRange = lspByteRange(
                    document.text, objectValue(rawEdit, "range"));
                const auto newText = rawEdit.find("newText");
                if (editFile.empty() or not editRange or
                    comparablePath(std::move(editPath)) != currentComparable or
                    newText == rawEdit.end() or not newText->is_string() or
                    newText->get_ref<const std::string &>().empty() or
                    editRange->first != editRange->second) {
                    valid = false;
                    break;
                }
                insertionOffsets.push_back(editRange->first);
                edits.push_back({
                    {"range", rawEdit["range"]},
                    {"newText", *newText}
                });
            }
            std::ranges::sort(insertionOffsets);
            if (std::adjacent_find(insertionOffsets.begin(),
                                   insertionOffsets.end()) !=
                insertionOffsets.end()) valid = false;
            if (not valid or edits.empty()) continue;

            const std::string key =
                stringValue(fix, "id") + "|" + edits.dump();
            if (not seen.insert(key).second) continue;
            Json action{
                {"title", title},
                {"kind", "quickfix"},
                {"diagnostics", Json::array({diagnostic})},
                {"isPreferred", true},
                {"edit", {
                    {"documentChanges", Json::array({
                        {
                            {"textDocument", {
                                {"uri", uri},
                                {"version", document.version}
                            }},
                            {"edits", std::move(edits)}
                        }
                    })}
                }}
            };
            const std::string fixId = stringValue(fix, "id");
            if (not fixId.empty()) action["data"] = {{"fixId", fixId}};
            actions.push_back(std::move(action));
        }
    }
    sendResult(id, actions);
}

void BaaLanguageServer::handleDocumentFormatting(const Json &id,
                                                 const Json &params)
{
    const std::string uri =
        stringValue(objectValue(params, "textDocument"), "uri");
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
    const std::string filePath = localPathForUri(uri);
    if (filePath.empty()) {
        sendError(id, InvalidParams,
                  "Formatting supports local file:// documents only.");
        return;
    }

    invalidateFormatRequests(
        uri, RequestCancelled,
        "A newer formatting request superseded this request.");
    std::uint64_t token = 0;
    {
        std::scoped_lock lock(m_formatMutex);
        token = m_nextFormatToken++;
        if (token == 0) token = m_nextFormatToken++;
        m_formatRequests.emplace(
            token,
            PendingFormatRequest{id, uri, document.version});
    }
    m_compiler.requestFormat({
        token,
        uri,
        filePath,
        document.text,
        document.version
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
    const bool projectIndexRequired =
        kind == SemanticReplyKind::Definition or
        kind == SemanticReplyKind::References or
        kind == SemanticReplyKind::PrepareRename or
        kind == SemanticReplyKind::Rename;
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
            cached->second.positionByte == positionByte and
            (not projectIndexRequired or
             cached->second.projectIndexIncluded)) {
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
                pending.positionByte == positionByte and
                pending.projectIndexRequired == projectIndexRequired) {
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
        pending.projectIndexRequired = projectIndexRequired;
        pending.replies.push_back(reply);
        compilerRequest.token = token;
        compilerRequest.uri = uri;
        compilerRequest.filePath = path;
        compilerRequest.text = document.text;
        compilerRequest.version = document.version;
        compilerRequest.positionByte = positionByte;
        compilerRequest.projectIndexRequired = projectIndexRequired;
        if (const ProjectPlan *plan =
                projectPlanForPath(pathFromUtf8(path))) {
            compilerRequest.projectWorkingDirectory =
                plan->workingDirectory;
            compilerRequest.includePaths = plan->includePaths;

            if (projectIndexRequired) {
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
                     plan->sourceFiles) {
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
            }
        } else if (projectIndexRequired) {
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
        std::scoped_lock lock(m_symbolRequestsMutex);
        const auto match = std::ranges::find_if(
            m_pendingWorkspaceSymbolRequests,
            [&idIt](const PendingWorkspaceSymbolRequest &request) {
                return request.id == *idIt;
            });
        if (match != m_pendingWorkspaceSymbolRequests.end()) {
            m_pendingWorkspaceSymbolRequests.erase(match);
            cancelled = true;
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
    std::uint64_t tokenRequestToCancel = 0;
    if (not cancelled) {
        std::scoped_lock lock(m_tokenRequestsMutex);
        for (auto request = m_tokenRequests.begin();
             request != m_tokenRequests.end(); ++request) {
            const auto match =
                std::ranges::find(request->second.ids, *idIt);
            if (match == request->second.ids.end()) continue;
            request->second.ids.erase(match);
            cancelled = true;
            if (request->second.ids.empty()) {
                tokenRequestToCancel = request->first;
                m_tokenRequests.erase(request);
            }
            break;
        }
    }
    if (tokenRequestToCancel != 0)
        m_compiler.cancelTokens(tokenRequestToCancel);
    std::uint64_t structureTokenToCancel = 0;
    if (not cancelled) {
        std::scoped_lock lock(m_structureMutex);
        for (auto request = m_structureRequests.begin();
             request != m_structureRequests.end(); ++request) {
            const auto reply = std::ranges::find_if(
                request->second.replies,
                [&idIt](const PendingStructureReply &candidate) {
                    return candidate.id == *idIt;
                });
            if (reply == request->second.replies.end()) continue;
            request->second.replies.erase(reply);
            cancelled = true;
            if (request->second.replies.empty()) {
                structureTokenToCancel = request->first;
                m_structureRequests.erase(request);
            }
            break;
        }
    }
    if (structureTokenToCancel != 0)
        m_compiler.cancelStructure(structureTokenToCancel);
    std::uint64_t inlayHintTokenToCancel = 0;
    if (not cancelled) {
        std::scoped_lock lock(m_inlayHintMutex);
        for (auto request = m_inlayHintRequests.begin();
             request != m_inlayHintRequests.end(); ++request) {
            const auto reply = std::ranges::find_if(
                request->second.replies,
                [&idIt](const PendingInlayHintReply &candidate) {
                    return candidate.id == *idIt;
                });
            if (reply == request->second.replies.end()) continue;
            request->second.replies.erase(reply);
            cancelled = true;
            if (request->second.replies.empty()) {
                inlayHintTokenToCancel = request->first;
                m_inlayHintRequests.erase(request);
            }
            break;
        }
    }
    if (inlayHintTokenToCancel != 0)
        m_compiler.cancelInlayHints(inlayHintTokenToCancel);
    std::uint64_t formatTokenToCancel = 0;
    if (not cancelled) {
        std::scoped_lock lock(m_formatMutex);
        for (auto request = m_formatRequests.begin();
             request != m_formatRequests.end(); ++request) {
            if (request->second.id != *idIt) continue;
            formatTokenToCancel = request->first;
            m_formatRequests.erase(request);
            cancelled = true;
            break;
        }
    }
    if (formatTokenToCancel != 0)
        m_compiler.cancelFormat(formatTokenToCancel);
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
        sendLogEvent("document.uri.unsupported", "document",
                     "This Baa-LSP version supports local file:// documents only.");
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
        sendLogEvent("compiler.analysis.failed", "compiler",
                     "Baa compiler analysis failed.");
        return;
    }
    publishDiagnostics(result.uri, result.version,
                       PositionEncoding::baaDiagnosticsToLsp(result.text, result.diagnostics));
    if (result.exitCode == 0) requestSymbolsForDocument(document, nullptr);
}

void BaaLanguageServer::onFormatFinished(BaaFormatResult result)
{
    PendingFormatRequest pending;
    {
        std::scoped_lock lock(m_formatMutex);
        const auto request = m_formatRequests.find(result.token);
        if (request == m_formatRequests.end()) return;
        pending = std::move(request->second);
        m_formatRequests.erase(request);
    }

    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(pending.uri) or
            m_documents.document(pending.uri).version != pending.version) {
            sendError(pending.id, ContentModified,
                      "Document changed before formatting was ready.");
            return;
        }
        document = m_documents.document(pending.uri);
    }
    if (not result.errorMessage.empty()) {
        sendError(pending.id, InternalError, result.errorMessage);
        return;
    }
    if (not result.changed or result.formattedText == document.text) {
        sendResult(pending.id, Json::array());
        return;
    }

    const Json end = PositionEncoding::utf16PositionForByteOffset(
        document.text, document.text.size());
    sendResult(pending.id, Json::array({
        {
            {"range", {
                {"start", {{"line", 0}, {"character", 0}}},
                {"end", end}
            }},
            {"newText", result.formattedText}
        }
    }));
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

    bool current = not pending.requireOpenDocument;
    if (pending.requireOpenDocument) {
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
        if (pending.workspaceIndex) {
            {
                std::scoped_lock lock(m_symbolRequestsMutex);
                m_workspaceSymbolIndex.insert_or_assign(
                    pending.uri,
                    WorkspaceSymbolIndexEntry{
                        pending.version,
                        pending.requireOpenDocument,
                        Json::array(),
                        result.errorMessage
                    });
            }
            sendLogEvent(
                "workspace.symbol.source-skipped", "workspace",
                "Workspace symbols skipped one Baa source.", 2);
            resolveWorkspaceSymbolRequests();
        }
        return;
    }
    if (pending.requireOpenDocument) {
        std::scoped_lock lock(m_symbolRequestsMutex);
        m_symbolCache.insert_or_assign(
            pending.uri,
            CachedSymbols{pending.version, result.text, result.symbols});
    }
    if (pending.workspaceIndex) {
        Json workspaceSymbols = Json::array();
        appendWorkspaceSymbols(
            workspaceSymbols,
            pending.uri,
            result.text,
            result.symbols,
            {});
        {
            std::scoped_lock lock(m_symbolRequestsMutex);
            m_workspaceSymbolIndex.insert_or_assign(
                pending.uri,
                WorkspaceSymbolIndexEntry{
                    pending.version,
                    pending.requireOpenDocument,
                    std::move(workspaceSymbols),
                    {}
                });
        }
    }
    const Json converted = PositionEncoding::baaSymbolsToLsp(result.text, result.symbols);
    for (const Json &id : pending.ids) sendResult(id, converted);
    if (pending.workspaceIndex) resolveWorkspaceSymbolRequests();
}

void BaaLanguageServer::onTokensFinished(BaaTokenResult result)
{
    PendingTokenRequest pending;
    {
        std::scoped_lock lock(m_tokenRequestsMutex);
        const auto request = m_tokenRequests.find(result.token);
        if (request == m_tokenRequests.end()) return;
        pending = std::move(request->second);
        m_tokenRequests.erase(request);
    }

    BaaDocument document;
    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(pending.uri) or
            m_documents.document(pending.uri).version != pending.version) {
            for (const Json &id : pending.ids) {
                sendError(id, ContentModified,
                          "Document changed before semantic tokens were ready.");
            }
            return;
        }
        document = m_documents.document(pending.uri);
    }
    if (not result.errorMessage.empty()) {
        for (const Json &id : pending.ids)
            sendError(id, InternalError, result.errorMessage);
        return;
    }

    {
        std::scoped_lock lock(m_tokenRequestsMutex);
        m_tokenCache.insert_or_assign(
            pending.uri,
            CachedTokens{pending.version, result.text, result.tokens});
    }
    const Json converted = {
        {"data", PositionEncoding::baaTokensToLspData(
            result.text, result.tokens)}
    };
    for (const Json &id : pending.ids) sendResult(id, converted);
}

void BaaLanguageServer::onStructureFinished(BaaStructureResult result)
{
    PendingStructureRequest pending;
    {
        std::scoped_lock lock(m_structureMutex);
        const auto request = m_structureRequests.find(result.token);
        if (request == m_structureRequests.end()) return;
        pending = std::move(request->second);
        m_structureRequests.erase(request);
    }

    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(pending.uri) or
            m_documents.document(pending.uri).version != pending.version) {
            for (const PendingStructureReply &reply : pending.replies) {
                sendError(reply.id, ContentModified,
                          "Document changed before structural editing data was ready.");
            }
            return;
        }
    }
    if (not result.errorMessage.empty()) {
        for (const PendingStructureReply &reply : pending.replies)
            sendError(reply.id, InternalError, result.errorMessage);
        return;
    }

    {
        std::scoped_lock lock(m_structureMutex);
        m_structureCache.insert_or_assign(
            pending.uri,
            CachedStructure{
                pending.version,
                result.text,
                result.complete,
                result.foldingRanges,
                result.selectionRanges
            });
    }
    for (const PendingStructureReply &reply : pending.replies) {
        if (reply.kind == StructureReplyKind::Folding) {
            sendResult(reply.id, PositionEncoding::baaFoldingRangesToLsp(
                result.text, result.foldingRanges));
        } else {
            sendResult(reply.id, PositionEncoding::baaSelectionRangesToLsp(
                result.text, result.selectionRanges, reply.positions));
        }
    }
}

void BaaLanguageServer::onInlayHintsFinished(BaaInlayHintResult result)
{
    PendingInlayHintRequest pending;
    {
        std::scoped_lock lock(m_inlayHintMutex);
        const auto request = m_inlayHintRequests.find(result.token);
        if (request == m_inlayHintRequests.end()) return;
        pending = std::move(request->second);
        m_inlayHintRequests.erase(request);
    }

    {
        std::scoped_lock lock(m_documentsMutex);
        if (not m_documents.contains(pending.uri) or
            m_documents.document(pending.uri).version != pending.version) {
            for (const PendingInlayHintReply &reply : pending.replies) {
                sendError(reply.id, ContentModified,
                          "Document changed before inlay hints were ready.");
            }
            return;
        }
    }
    if (not result.errorMessage.empty()) {
        for (const PendingInlayHintReply &reply : pending.replies)
            sendError(reply.id, InternalError, result.errorMessage);
        return;
    }

    {
        std::scoped_lock lock(m_inlayHintMutex);
        m_inlayHintCache.insert_or_assign(
            pending.uri,
            CachedInlayHints{
                pending.version,
                result.text,
                result.complete,
                result.hints
            });
    }
    for (const PendingInlayHintReply &reply : pending.replies) {
        sendResult(reply.id, inlayHintsForRange(
            result.text, result.hints, reply.startByte, reply.endByte,
            result.complete));
    }
}

Json BaaLanguageServer::workspaceSymbolResult(const std::string &query)
{
    Json result = Json::array();
    std::unordered_set<std::string> seen;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        for (const auto &[uri, entry] : m_workspaceSymbolIndex) {
            (void)uri;
            if (not entry.symbols.is_array()) continue;
            for (const Json &symbol : entry.symbols) {
                if (not symbol.is_object()) continue;
                const std::string name = symbol.value("name", "");
                if (name.empty() or
                    not workspaceSymbolMatches(name, query))
                    continue;
                const std::string key =
                    name + "\n" +
                    symbol.value("containerName", "") + "\n" +
                    symbol.value("location", Json::object()).dump();
                if (seen.insert(key).second) result.push_back(symbol);
            }
        }
    }
    std::vector<Json> sorted(result.begin(), result.end());
    std::ranges::sort(sorted, [](const Json &left, const Json &right) {
        const std::string leftName = left.value("name", "");
        const std::string rightName = right.value("name", "");
        if (leftName != rightName) return leftName < rightName;
        const std::string leftContainer =
            left.value("containerName", "");
        const std::string rightContainer =
            right.value("containerName", "");
        if (leftContainer != rightContainer)
            return leftContainer < rightContainer;
        return left.value("location", Json::object()).dump() <
               right.value("location", Json::object()).dump();
    });
    result = Json::array();
    for (Json &symbol : sorted) result.push_back(std::move(symbol));
    return result;
}

void BaaLanguageServer::resolveWorkspaceSymbolRequests()
{
    std::vector<std::pair<Json, std::string>> ready;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        for (auto request = m_pendingWorkspaceSymbolRequests.begin();
             request != m_pendingWorkspaceSymbolRequests.end();) {
            bool complete = true;
            for (const WorkspaceSymbolSourceVersion &source :
                 request->sources) {
                const auto indexed =
                    m_workspaceSymbolIndex.find(source.uri);
                if (indexed == m_workspaceSymbolIndex.end() or
                    indexed->second.version != source.version or
                    indexed->second.openDocument != source.openDocument) {
                    complete = false;
                    break;
                }
            }
            if (not complete) {
                ++request;
                continue;
            }
            ready.emplace_back(
                std::move(request->id),
                std::move(request->query));
            request = m_pendingWorkspaceSymbolRequests.erase(request);
        }
    }
    for (auto &[id, query] : ready)
        sendResult(id, workspaceSymbolResult(query));
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
        sendLogEvent("compiler.completion-data.failed", "compiler",
                     "Baa compiler completion data could not be loaded.");
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
        case SemanticReplyKind::Completion:
        {
            Json metadata;
            {
                std::scoped_lock lock(m_completionMutex);
                metadata = m_completionItems;
            }
            const std::size_t cursor =
                std::min(query.positionByte, query.text.size());
            const std::size_t prefixStart =
                completionPrefixStart(query.text, cursor);
            return {
                {"isIncomplete", not query.completionComplete},
                {"items", buildCompletionItems(
                    query.text, prefixStart, cursor,
                    metadata, query.completionItems)}
            };
        }
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
    for (const std::string &warning : result.warnings) {
        (void)warning;
        sendLogEvent(
            "compiler.semantic.warning", "compiler",
            "Baa compiler reported a warning while serving a semantic request.",
            2);
    }

    CachedSemanticQuery completed;
    completed.version = pending.version;
    completed.positionByte = pending.positionByte;
    completed.text = std::move(result.text);
    completed.hover = std::move(result.hover);
    completed.signatureHelp = std::move(result.signatureHelp);
    completed.definition = std::move(result.definition);
    completed.references = std::move(result.references);
    completed.completionItems = std::move(result.completionItems);
    completed.completionComplete = result.completionComplete;
    completed.symbol = std::move(result.symbol);
    completed.projectOccurrences = std::move(result.projectOccurrences);
    completed.projectIndexOccurrences =
        std::move(result.projectIndexOccurrences);
    completed.projectIndexIncluded = pending.projectIndexRequired;
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

void BaaLanguageServer::invalidateTokenRequests(const std::string &uri,
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
        std::scoped_lock lock(m_tokenRequestsMutex);
        for (auto request = m_tokenRequests.begin();
             request != m_tokenRequests.end();) {
            if (uri.empty() or request->second.uri == uri) {
                cancelled.push_back(
                    {request->first, std::move(request->second.ids)});
                request = m_tokenRequests.erase(request);
            } else {
                ++request;
            }
        }
    }
    for (const CancelledRequest &request : cancelled) {
        m_compiler.cancelTokens(request.token);
        for (const Json &id : request.ids) sendError(id, code, message);
    }
}

void BaaLanguageServer::invalidateStructureRequests(
    const std::string &uri,
    int code,
    const std::string &message)
{
    struct CancelledRequest
    {
        std::uint64_t token{};
        std::vector<PendingStructureReply> replies;
    };
    std::vector<CancelledRequest> cancelled;
    {
        std::scoped_lock lock(m_structureMutex);
        for (auto request = m_structureRequests.begin();
             request != m_structureRequests.end();) {
            if (uri.empty() or request->second.uri == uri) {
                cancelled.push_back(
                    {request->first, std::move(request->second.replies)});
                request = m_structureRequests.erase(request);
            } else {
                ++request;
            }
        }
    }
    for (const CancelledRequest &request : cancelled) {
        m_compiler.cancelStructure(request.token);
        for (const PendingStructureReply &reply : request.replies)
            sendError(reply.id, code, message);
    }
}

void BaaLanguageServer::invalidateInlayHintRequests(
    const std::string &uri,
    int code,
    const std::string &message)
{
    struct CancelledRequest
    {
        std::uint64_t token{};
        std::vector<PendingInlayHintReply> replies;
    };
    std::vector<CancelledRequest> cancelled;
    {
        std::scoped_lock lock(m_inlayHintMutex);
        for (auto request = m_inlayHintRequests.begin();
             request != m_inlayHintRequests.end();) {
            if (uri.empty() or request->second.uri == uri) {
                cancelled.push_back(
                    {request->first, std::move(request->second.replies)});
                request = m_inlayHintRequests.erase(request);
            } else {
                ++request;
            }
        }
    }
    for (const CancelledRequest &request : cancelled) {
        m_compiler.cancelInlayHints(request.token);
        for (const PendingInlayHintReply &reply : request.replies)
            sendError(reply.id, code, message);
    }
}

void BaaLanguageServer::invalidateWorkspaceSymbolRequests(
    int code,
    const std::string &message)
{
    std::vector<Json> ids;
    {
        std::scoped_lock lock(m_symbolRequestsMutex);
        ids.reserve(m_pendingWorkspaceSymbolRequests.size());
        for (PendingWorkspaceSymbolRequest &request :
             m_pendingWorkspaceSymbolRequests) {
            ids.push_back(std::move(request.id));
        }
        m_pendingWorkspaceSymbolRequests.clear();
    }
    for (const Json &id : ids) sendError(id, code, message);
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

void BaaLanguageServer::invalidateFormatRequests(const std::string &uri,
                                                 int code,
                                                 const std::string &message)
{
    struct CancelledRequest
    {
        std::uint64_t token{};
        Json id;
    };
    std::vector<CancelledRequest> cancelled;
    {
        std::scoped_lock lock(m_formatMutex);
        for (auto request = m_formatRequests.begin();
             request != m_formatRequests.end();) {
            if (uri.empty() or request->second.uri == uri) {
                cancelled.push_back(
                    {request->first, std::move(request->second.id)});
                request = m_formatRequests.erase(request);
            } else {
                ++request;
            }
        }
    }
    for (const CancelledRequest &request : cancelled) {
        m_compiler.cancelFormat(request.token);
        sendError(request.id, code, message);
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
    {
        std::scoped_lock lock(m_diagnosticsMutex);
        m_publishedDiagnostics[uri] = {version, diagnostics};
    }
    Json params{{"uri", uri}, {"diagnostics", diagnostics}};
    if (version > 0) params["version"] = version;
    sendJson({{"jsonrpc", "2.0"},
              {"method", "textDocument/publishDiagnostics"},
              {"params", std::move(params)}});
}

void BaaLanguageServer::sendLogEvent(std::string_view event,
                                     std::string_view component,
                                     const std::string &message,
                                     int type,
                                     Json data)
{
    std::scoped_lock lock(m_logMutex);
    if (m_structuredLogsEnabled) {
        sendJson({
            {"jsonrpc", "2.0"},
            {"method", "baa/logEvent"},
            {"params", {
                {"schema_version", std::string(StructuredLogSchema)},
                {"sequence", m_nextLogSequence++},
                {"severity", std::string(logSeverity(type))},
                {"component", std::string(component)},
                {"event", std::string(event)},
                {"message", message},
                {"data", data.is_object() ? std::move(data) : Json::object()}
            }}
        });
        return;
    }
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
