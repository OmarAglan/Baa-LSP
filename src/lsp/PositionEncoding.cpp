#include "lsp/PositionEncoding.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

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
        if (not data.empty()) result["data"] = std::move(data);
        converted.push_back(std::move(result));
    }
    return converted;
}
