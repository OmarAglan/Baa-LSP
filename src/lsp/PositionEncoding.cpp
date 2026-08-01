#include "lsp/PositionEncoding.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

namespace {
std::size_t clampedUtf8Boundary(std::string_view text, std::size_t offset)
{
    offset = std::min(offset, text.size());
    while (offset > 0 and offset < text.size() and
           (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u) {
        --offset;
    }
    return offset;
}

std::pair<std::uint32_t, std::size_t> decodeUtf8(std::string_view text, std::size_t offset)
{
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80u) return {first, 1};

    std::size_t count{};
    std::uint32_t codePoint{};
    if ((first & 0xE0u) == 0xC0u) {
        count = 2;
        codePoint = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {
        count = 3;
        codePoint = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {
        count = 4;
        codePoint = first & 0x07u;
    } else {
        return {0xFFFDu, 1};
    }
    if (offset + count > text.size()) return {0xFFFDu, 1};
    for (std::size_t index = 1; index < count; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xC0u) != 0x80u) return {0xFFFDu, 1};
        codePoint = (codePoint << 6u) | (next & 0x3Fu);
    }
    return {codePoint, count};
}

int lspSeverity(std::string severity)
{
    std::ranges::transform(severity, severity.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (severity == "warning") return 2;
    if (severity == "information") return 3;
    if (severity == "hint") return 4;
    return 1;
}

std::int64_t integerValue(const Json &object, std::string_view key, std::int64_t fallback)
{
    const auto it = object.find(std::string(key));
    return it != object.end() and it->is_number_integer() ? it->get<std::int64_t>() : fallback;
}

int symbolKind(const Json &symbol)
{
    const std::string kind = symbol.value("kind", "");
    if (kind == "function") return 12;
    if (kind == "parameter") return 13;
    if (kind == "variable") {
        const Json modifiers = symbol.value("modifiers", Json::object());
        return modifiers.value("const", false) ? 14 : 13;
    }
    if (kind == "array") return 18;
    if (kind == "type-alias") return 26;
    if (kind == "enum") return 10;
    if (kind == "enum-member") return 22;
    if (kind == "struct" or kind == "union") return 23;
    if (kind == "field") return 8;
    return 13;
}

int semanticTokenType(const std::string &kind)
{
    if (kind == "type") return 0;
    if (kind == "directive") return 1;
    if (kind == "keyword") return 2;
    if (kind == "modifier") return 3;
    if (kind == "comment") return 4;
    if (kind == "string" or kind == "character") return 5;
    if (kind == "number") return 6;
    if (kind == "operator") return 7;
    if (kind == "function") return 8;
    if (kind == "variable" or kind == "constant" or kind == "array") return 9;
    if (kind == "parameter") return 10;
    if (kind == "field") return 11;
    if (kind == "enum-member") return 12;
    if (kind == "type-alias" or kind == "enum" or kind == "struct" or
        kind == "union") return 0;
    return -1;
}

std::string typeDisplay(const Json &symbol, std::string_view field)
{
    const auto it = symbol.find(std::string(field));
    if (it == symbol.end() or not it->is_object()) return {};
    return it->value("display", "");
}

std::string symbolDetail(const Json &symbol)
{
    const std::string kind = symbol.value("kind", "");
    if (kind == "function") {
        const std::string display = typeDisplay(symbol, "return_type");
        return display.empty() ? std::string{} : "-> " + display;
    }
    if (kind == "array") {
        const std::string display = typeDisplay(symbol, "element_type");
        return display.empty() ? std::string{} : display + "[]";
    }
    if (kind == "type-alias") {
        const std::string display = typeDisplay(symbol, "target_type");
        return display.empty() ? std::string{} : "= " + display;
    }
    return typeDisplay(symbol, "type");
}

Json convertSymbol(std::string_view text, const Json &symbol)
{
    if (not symbol.is_object()) return nullptr;
    const std::string name = symbol.value("name", "");
    if (name.empty()) return nullptr;

    const Json span = symbol.value("span", Json::object());
    const Json rawStart = span.value("start", Json::object());
    const Json rawEnd = span.value("end", Json::object());
    const std::int64_t rawStartByte = integerValue(rawStart, "byte", -1);
    const std::size_t startByte = rawStartByte >= 0
        ? static_cast<std::size_t>(rawStartByte)
        : PositionEncoding::byteOffsetForOneBasedLocation(
              text,
              static_cast<int>(integerValue(rawStart, "line", 1)),
              static_cast<int>(integerValue(rawStart, "column", 1)));
    const std::int64_t rawEndByte = integerValue(rawEnd, "byte", -1);
    const std::size_t endByte = std::max(
        startByte,
        rawEndByte >= 0
            ? static_cast<std::size_t>(rawEndByte)
            : PositionEncoding::byteOffsetForOneBasedLocation(
                  text,
                  static_cast<int>(integerValue(rawEnd, "line",
                                                integerValue(rawStart, "line", 1))),
                  static_cast<int>(integerValue(rawEnd, "column",
                                                integerValue(rawStart, "column", 1)))));
    const Json range{
        {"start", PositionEncoding::utf16PositionForByteOffset(text, startByte)},
        {"end", PositionEncoding::utf16PositionForByteOffset(text, endByte)}
    };

    Json converted{
        {"name", name},
        {"kind", symbolKind(symbol)},
        {"range", range},
        {"selectionRange", range}
    };
    const std::string detail = symbolDetail(symbol);
    if (not detail.empty()) converted["detail"] = detail;

    const Json children = symbol.value("children", Json::array());
    if (children.is_array()) {
        Json convertedChildren = Json::array();
        for (const Json &child : children) {
            Json convertedChild = convertSymbol(text, child);
            if (not convertedChild.is_null()) {
                convertedChildren.push_back(std::move(convertedChild));
            }
        }
        if (not convertedChildren.empty()) {
            converted["children"] = std::move(convertedChildren);
        }
    }
    return converted;
}
}

Json PositionEncoding::utf16PositionForByteOffset(std::string_view text,
                                                  std::size_t utf8ByteOffset)
{
    const std::size_t boundary = clampedUtf8Boundary(text, utf8ByteOffset);
    int line = 0;
    int character = 0;
    for (std::size_t offset = 0; offset < boundary;) {
        if (text[offset] == '\n') {
            ++line;
            character = 0;
            ++offset;
            continue;
        }
        const auto [codePoint, bytes] = decodeUtf8(text, offset);
        character += codePoint > 0xFFFFu ? 2 : 1;
        offset += bytes;
    }
    return Json{{"line", line}, {"character", character}};
}

std::size_t PositionEncoding::utf8ByteOffsetForUtf16Position(std::string_view text,
                                                             int zeroBasedLine,
                                                             int utf16Character)
{
    const int wantedLine = std::max(0, zeroBasedLine);
    const int wantedCharacter = std::max(0, utf16Character);
    std::size_t offset = 0;
    int currentLine = 0;
    while (offset < text.size() and currentLine < wantedLine) {
        if (text[offset++] == '\n') ++currentLine;
    }
    if (currentLine != wantedLine) return text.size();

    int currentCharacter = 0;
    while (offset < text.size() and text[offset] != '\n') {
        const auto [codePoint, bytes] = decodeUtf8(text, offset);
        const int units = codePoint > 0xFFFFu ? 2 : 1;
        if (currentCharacter + units > wantedCharacter) break;
        currentCharacter += units;
        offset += bytes;
        if (currentCharacter == wantedCharacter) break;
    }
    return offset;
}

std::size_t PositionEncoding::byteOffsetForOneBasedLocation(std::string_view text,
                                                            int line,
                                                            int column)
{
    const int wantedLine = std::max(1, line);
    std::size_t offset = 0;
    int currentLine = 1;
    while (offset < text.size() and currentLine < wantedLine) {
        if (text[offset++] == '\n') ++currentLine;
    }
    if (currentLine != wantedLine) return text.size();

    const std::size_t withinLine = static_cast<std::size_t>(std::max(0, column - 1));
    const std::size_t newline = text.find('\n', offset);
    const std::size_t lineEnd = newline == std::string_view::npos ? text.size() : newline;
    return std::min(offset + withinLine, lineEnd);
}

Json PositionEncoding::baaDiagnosticsToLsp(std::string_view text, const Json &diagnostics)
{
    Json converted = Json::array();
    if (not diagnostics.is_array()) return converted;

    for (const Json &item : diagnostics) {
        if (not item.is_object()) continue;
        const std::string message = item.value("message", "");
        if (message.empty()) continue;

        const Json span = item.value("span", Json::object());
        const Json rawStart = span.value("start", Json::object());
        const Json rawEnd = span.value("end", Json::object());
        const std::int64_t itemLine = integerValue(item, "line", 1);
        const std::int64_t itemColumn = integerValue(item, "column", 1);

        const std::int64_t rawStartByte = integerValue(rawStart, "byte", -1);
        const std::size_t startByte = rawStartByte >= 0
            ? static_cast<std::size_t>(rawStartByte)
            : byteOffsetForOneBasedLocation(text,
                  static_cast<int>(integerValue(rawStart, "line", itemLine)),
                  static_cast<int>(integerValue(rawStart, "column", itemColumn)));
        const std::int64_t rawEndByte = integerValue(rawEnd, "byte", -1);
        const std::size_t endByte = std::max(startByte, rawEndByte >= 0
            ? static_cast<std::size_t>(rawEndByte)
            : byteOffsetForOneBasedLocation(text,
                  static_cast<int>(integerValue(rawEnd, "line",
                                                integerValue(rawStart, "line", 1))),
                  static_cast<int>(integerValue(rawEnd, "column",
                                                integerValue(rawStart, "column", 1)))));

        Json result{
            {"range", {
                {"start", utf16PositionForByteOffset(text, startByte)},
                {"end", utf16PositionForByteOffset(text, endByte)}
            }},
            {"severity", lspSeverity(item.value("severity", "error"))},
            {"source", "باء"},
            {"message", message}
        };
        if (item.contains("code")) result["code"] = item["code"];

        Json data = Json::object();
        if (item.contains("category")) data["category"] = item["category"];
        if (item.contains("hint") and not item["hint"].is_null()) data["hint"] = item["hint"];
        Json convertedFixes = Json::array();
        const Json fixes = item.value("fixes", Json::array());
        if (fixes.is_array()) {
            for (const Json &fix : fixes) {
                if (not fix.is_object() or
                    fix.value("kind", "") != "quickfix" or
                    fix.value("applicability", "") != "safe" or
                    fix.value("title", "").empty()) continue;
                const Json rawEdits = fix.value("edits", Json::array());
                if (not rawEdits.is_array() or rawEdits.empty()) continue;

                Json edits = Json::array();
                bool valid = true;
                for (const Json &rawEdit : rawEdits) {
                    if (not rawEdit.is_object() or
                        not rawEdit.contains("new_text") or
                        not rawEdit["new_text"].is_string()) {
                        valid = false;
                        break;
                    }
                    const Json editSpan = rawEdit.value("span", Json::object());
                    const Json editStart = editSpan.value("start", Json::object());
                    const Json editEnd = editSpan.value("end", Json::object());
                    const std::int64_t editStartByte =
                        integerValue(editStart, "byte", -1);
                    const std::int64_t editEndByte =
                        integerValue(editEnd, "byte", -1);
                    if (editStartByte < 0 or editEndByte < editStartByte or
                        static_cast<std::size_t>(editEndByte) > text.size()) {
                        valid = false;
                        break;
                    }
                    const Json startPosition = utf16PositionForByteOffset(
                        text, static_cast<std::size_t>(editStartByte));
                    const Json endPosition = utf16PositionForByteOffset(
                        text, static_cast<std::size_t>(editEndByte));
                    if (utf8ByteOffsetForUtf16Position(
                            text,
                            startPosition.value("line", -1),
                            startPosition.value("character", -1)) !=
                            static_cast<std::size_t>(editStartByte) or
                        utf8ByteOffsetForUtf16Position(
                            text,
                            endPosition.value("line", -1),
                            endPosition.value("character", -1)) !=
                            static_cast<std::size_t>(editEndByte)) {
                        valid = false;
                        break;
                    }
                    Json edit{
                        {"file", rawEdit.value("file", "")},
                        {"range", {
                            {"start", startPosition},
                            {"end", endPosition}
                        }},
                        {"newText", rawEdit["new_text"]}
                    };
                    edits.push_back(std::move(edit));
                }
                if (not valid or edits.empty()) continue;

                Json convertedFix{
                    {"id", fix.value("id", "")},
                    {"title", fix["title"]},
                    {"kind", "quickfix"},
                    {"applicability", "safe"},
                    {"edits", std::move(edits)}
                };
                convertedFixes.push_back(std::move(convertedFix));
            }
        }
        if (not convertedFixes.empty()) data["fixes"] = std::move(convertedFixes);
        if (not data.empty()) result["data"] = std::move(data);
        converted.push_back(std::move(result));
    }
    return converted;
}

Json PositionEncoding::baaTokensToLspData(std::string_view text,
                                          const Json &tokens)
{
    struct RawSegment
    {
        std::size_t start{};
        std::size_t end{};
        int type{};
    };
    struct SemanticToken
    {
        int line{};
        int character{};
        int length{};
        int type{};
    };

    std::vector<RawSegment> segments;
    if (not tokens.is_array()) return Json::array();
    for (const Json &token : tokens) {
        if (not token.is_object()) continue;
        const int type = semanticTokenType(token.value("kind", ""));
        if (type < 0) continue;
        const Json span = token.value("span", Json::object());
        const Json rawStart = span.value("start", Json::object());
        const Json rawEnd = span.value("end", Json::object());
        const std::int64_t startValue = integerValue(rawStart, "byte", -1);
        const std::int64_t endValue = integerValue(rawEnd, "byte", -1);
        if (startValue < 0 or endValue <= startValue) continue;
        std::size_t segmentStart = static_cast<std::size_t>(startValue);
        const std::size_t tokenEnd = static_cast<std::size_t>(endValue);
        if (segmentStart >= text.size() or tokenEnd > text.size()) continue;

        while (segmentStart < tokenEnd) {
            const std::size_t newline = text.find('\n', segmentStart);
            std::size_t segmentEnd =
                newline == std::string_view::npos or newline >= tokenEnd
                    ? tokenEnd
                    : newline;
            if (segmentEnd > segmentStart and text[segmentEnd - 1] == '\r')
                --segmentEnd;
            if (segmentEnd > segmentStart)
                segments.push_back({segmentStart, segmentEnd, type});
            if (newline == std::string_view::npos or newline >= tokenEnd)
                break;
            segmentStart = newline + 1;
        }
    }

    std::ranges::sort(segments, [](const RawSegment &left,
                                  const RawSegment &right) {
        if (left.start != right.start) return left.start < right.start;
        if (left.end != right.end) return left.end < right.end;
        return left.type < right.type;
    });

    std::vector<SemanticToken> converted;
    converted.reserve(segments.size());
    std::size_t scanOffset = 0;
    int scanLine = 0;
    int scanCharacter = 0;
    auto advanceTo = [&](std::size_t target) {
        target = std::min(target, text.size());
        while (scanOffset < target) {
            if (text[scanOffset] == '\n') {
                ++scanLine;
                scanCharacter = 0;
                ++scanOffset;
                continue;
            }
            const auto [codePoint, length] = decodeUtf8(text, scanOffset);
            scanCharacter += codePoint > 0xFFFFu ? 2 : 1;
            scanOffset += std::min(length, target - scanOffset);
        }
    };
    for (const RawSegment &segment : segments) {
        if (segment.start < scanOffset) continue;
        advanceTo(segment.start);
        const int startLine = scanLine;
        const int startCharacter = scanCharacter;
        advanceTo(segment.end);
        if (scanLine == startLine and scanCharacter > startCharacter) {
            converted.push_back({
                startLine,
                startCharacter,
                scanCharacter - startCharacter,
                segment.type
            });
        }
    }

    Json data = Json::array();
    int previousLine = 0;
    int previousCharacter = 0;
    for (const SemanticToken &token : converted) {
        const int deltaLine = token.line - previousLine;
        const int deltaCharacter = deltaLine == 0
            ? token.character - previousCharacter
            : token.character;
        if (deltaLine < 0 or deltaCharacter < 0) continue;
        data.push_back(deltaLine);
        data.push_back(deltaCharacter);
        data.push_back(token.length);
        data.push_back(token.type);
        data.push_back(0);
        previousLine = token.line;
        previousCharacter = token.character;
    }
    return data;
}

Json PositionEncoding::baaSymbolsToLsp(std::string_view text, const Json &symbols)
{
    Json converted = Json::array();
    if (not symbols.is_array()) return converted;
    for (const Json &symbol : symbols) {
        Json item = convertSymbol(text, symbol);
        if (not item.is_null()) converted.push_back(std::move(item));
    }
    return converted;
}

Json PositionEncoding::baaSemanticHoverToLsp(std::string_view text,
                                             const Json &hover)
{
    if (not hover.is_object()) return nullptr;
    const std::string display = hover.value("display", "");
    if (display.empty()) return nullptr;

    const Json span = hover.value("range", Json::object());
    const Json rawStart = span.value("start", Json::object());
    const Json rawEnd = span.value("end", Json::object());
    const std::int64_t rawStartByte = integerValue(rawStart, "byte", -1);
    const std::int64_t rawEndByte = integerValue(rawEnd, "byte", -1);
    if (rawStartByte < 0 or rawEndByte < rawStartByte) return nullptr;

    std::string markdown = "```baa\n" + display + "\n```";
    const std::string description = hover.value("description", "");
    if (not description.empty()) markdown += "\n\n" + description;

    return Json{
        {"contents", {{"kind", "markdown"}, {"value", std::move(markdown)}}},
        {"range", {
            {"start", utf16PositionForByteOffset(
                text, static_cast<std::size_t>(rawStartByte))},
            {"end", utf16PositionForByteOffset(
                text, static_cast<std::size_t>(rawEndByte))}
        }}
    };
}

Json PositionEncoding::baaSignatureHelpToLsp(const Json &signatureHelp)
{
    if (not signatureHelp.is_object()) return nullptr;
    const std::string label = signatureHelp.value("label", "");
    if (label.empty()) return nullptr;
    Json parameters = signatureHelp.value("parameters", Json::array());
    if (not parameters.is_array()) parameters = Json::array();

    return Json{
        {"signatures", Json::array({
            {{"label", label}, {"parameters", std::move(parameters)}}
        })},
        {"activeSignature", 0},
        {"activeParameter", std::max(0, signatureHelp.value("active_parameter", 0))}
    };
}

Json PositionEncoding::baaLocationToLsp(std::string_view text,
                                        std::string_view uri,
                                        const Json &location)
{
    if (not location.is_object() or uri.empty()) return nullptr;
    const Json span = location.value("range", Json::object());
    const Json rawStart = span.value("start", Json::object());
    const Json rawEnd = span.value("end", Json::object());
    if (not rawStart.is_object() or not rawEnd.is_object()) return nullptr;

    const std::int64_t rawStartByte = integerValue(rawStart, "byte", -1);
    const std::size_t startByte = rawStartByte >= 0
        ? static_cast<std::size_t>(rawStartByte)
        : byteOffsetForOneBasedLocation(
              text,
              static_cast<int>(integerValue(rawStart, "line", 1)),
              static_cast<int>(integerValue(rawStart, "column", 1)));
    const std::int64_t rawEndByte = integerValue(rawEnd, "byte", -1);
    const std::size_t endByte = std::max(
        startByte,
        rawEndByte >= 0
            ? static_cast<std::size_t>(rawEndByte)
            : byteOffsetForOneBasedLocation(
                  text,
                  static_cast<int>(integerValue(
                      rawEnd, "line", integerValue(rawStart, "line", 1))),
                  static_cast<int>(integerValue(
                      rawEnd, "column", integerValue(rawStart, "column", 1)))));

    return Json{
        {"uri", std::string(uri)},
        {"range", {
            {"start", utf16PositionForByteOffset(text, startByte)},
            {"end", utf16PositionForByteOffset(text, endByte)}
        }}
    };
}
