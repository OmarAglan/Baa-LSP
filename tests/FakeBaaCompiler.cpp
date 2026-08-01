#include "lsp/Json.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
std::vector<std::string> commandLineArguments(int argc, char **argv)
{
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    int wideCount = 0;
    wchar_t **wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (not wideArguments) return {};
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(wideCount));
    for (int index = 0; index < wideCount; ++index) {
        const int bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[index], -1,
            nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            LocalFree(wideArguments);
            return {};
        }
        std::string encoded(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wideArguments[index], -1,
                encoded.data(), bytes, nullptr, nullptr) != bytes) {
            LocalFree(wideArguments);
            return {};
        }
        encoded.pop_back();
        arguments.push_back(std::move(encoded));
    }
    LocalFree(wideArguments);
    return arguments;
#else
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
        arguments.emplace_back(argv[index]);
    return arguments;
#endif
}
}

int main(int argc, char **argv)
{
    const std::vector<std::string> arguments = commandLineArguments(argc, argv);
    if (arguments.empty()) return 2;
    const std::string source((std::istreambuf_iterator<char>(std::cin)),
                             std::istreambuf_iterator<char>());
    bool dumpSymbols = false;
    bool dumpTokens = false;
    bool dumpStructure = false;
    bool completionData = false;
    bool formatJson = false;
    bool semanticQuery = false;
    bool semanticIndex = false;
    std::string logicalFile = "رئيسي.baa";
    std::size_t positionByte = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--dump-symbols=json") {
            dumpSymbols = true;
        } else if (argument == "--dump-tokens=json") {
            dumpTokens = true;
        } else if (argument == "--dump-structure=json") {
            dumpStructure = true;
        } else if (argument == "--completion-data=json") {
            completionData = true;
        } else if (argument == "--format=json") {
            formatJson = true;
        } else if (argument == "--semantic-query=json") {
            semanticQuery = true;
        } else if (argument == "--semantic-index=json") {
            semanticIndex = true;
        } else if (argument.starts_with("--position-byte=")) {
            positionByte = static_cast<std::size_t>(
                std::stoull(std::string(argument.substr(16))));
        } else if (argument.starts_with("--source-stdin=")) {
            logicalFile = std::string(argument.substr(15));
        }
    }
    if (dumpStructure) {
        auto location = [&source](std::size_t byte) {
            int line = 1;
            int column = 1;
            for (std::size_t index = 0; index < byte and index < source.size();
                 ++index) {
                if (source[index] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            return Json{{"line", line}, {"column", column}, {"byte", byte}};
        };
        auto range = [&location](std::string_view kind,
                                 std::size_t start,
                                 std::size_t end) {
            return Json{
                {"kind", kind},
                {"span", {
                    {"start", location(start)},
                    {"end", location(end)}
                }}
            };
        };

        Json foldingRanges = Json::array();
        Json selectionRanges = Json::array();
        bool complete = true;
        const std::size_t opening = source.find('{');
        const std::size_t closing = source.rfind('}');
        if (opening != std::string::npos and closing > opening) {
            if (location(opening)["line"] != location(closing + 1)["line"])
                foldingRanges.push_back(range("region", opening, closing + 1));
            if (closing > opening + 1)
                selectionRanges.push_back(
                    range("content", opening + 1, closing));
            selectionRanges.push_back(range("group", opening, closing + 1));
            selectionRanges.push_back(range("construct", 0, closing + 1));
        } else if (opening != std::string::npos or closing != std::string::npos) {
            complete = false;
        }
        if (not source.empty())
            selectionRanges.push_back(range("document", 0, source.size()));
        auto sortRanges = [](Json &ranges) {
            std::vector<Json> sorted(ranges.begin(), ranges.end());
            std::ranges::sort(sorted, [](const Json &left, const Json &right) {
                const std::size_t leftStart =
                    left["span"]["start"]["byte"].get<std::size_t>();
                const std::size_t rightStart =
                    right["span"]["start"]["byte"].get<std::size_t>();
                if (leftStart != rightStart) return leftStart < rightStart;
                const std::size_t leftEnd =
                    left["span"]["end"]["byte"].get<std::size_t>();
                const std::size_t rightEnd =
                    right["span"]["end"]["byte"].get<std::size_t>();
                if (leftEnd != rightEnd) return leftEnd > rightEnd;
                return left["kind"].get<std::string>() <
                       right["kind"].get<std::string>();
            });
            ranges = Json::array();
            for (Json &item : sorted) ranges.push_back(std::move(item));
        };
        sortRanges(foldingRanges);
        sortRanges(selectionRanges);

        std::cout << Json{
            {"schema_version", "structure-json-v1"},
            {"compiler_version", "test"},
            {"language", "baa"},
            {"file", logicalFile},
            {"position_encoding", "utf-8-bytes"},
            {"source_bytes", source.size()},
            {"complete", complete},
            {"folding_ranges", std::move(foldingRanges)},
            {"selection_ranges", std::move(selectionRanges)}
        }.dump();
        return 0;
    }
    if (formatJson) {
        const std::string formatted =
            "صحيح الرئيسية() {\n"
            "    مفقود = ١.\n"
            "}\n";
        std::cout << Json{
            {"schema_version", "format-json-v1"},
            {"compiler_version", "test"},
            {"language", "baa"},
            {"file", logicalFile},
            {"position_encoding", "utf-8-bytes"},
            {"line_ending", "lf"},
            {"indent_width", 4},
            {"insert_spaces", true},
            {"source_bytes", source.size()},
            {"formatted_bytes", formatted.size()},
            {"changed", formatted != source},
            {"formatted_text", formatted}
        }.dump();
        return 0;
    }
    if (completionData) {
        const Json items = Json::array({
            {{"label", "صحيح"}, {"kind", "type"}, {"detail", "نوع عدد صحيح"},
             {"documentation", "النوع الصحيح الافتراضي في باء."},
             {"filter_text", "صحيح"}, {"insert_text", "صحيح"},
             {"insert_text_format", "plain"}},
            {{"label", "الرئيسية (دالة)"}, {"kind", "snippet"},
             {"detail", "قالب نقطة بداية البرنامج"}, {"filter_text", "الرئيسية"},
             {"insert_text", "صحيح الرئيسية() {\n\t${0}\n\tإرجع ٠.\n}"},
             {"insert_text_format", "snippet"}},
            {{"label", "#تضمين"}, {"kind", "directive"},
             {"detail", "تضمين ملف ترويسة"}, {"filter_text", "#تضمين"},
             {"insert_text", "#تضمين \"${1:ملف.baahd}\""},
             {"insert_text_format", "snippet"}},
        });
        std::cout << Json{{"schema_version", "completion-data-json-v1"},
                          {"compiler_version", "test"}, {"language", "baa"},
                          {"items", items}}.dump();
        return 0;
    }
    if (semanticQuery) {
        const std::string name = "اجمع";
        const std::size_t start = source.find(name);
        Json hover = nullptr;
        Json signature = nullptr;
        Json definition = nullptr;
        Json references = Json::array();
        const Json completionItems = Json::array({
            {
                {"label", "الرئيسية"},
                {"kind", "function"},
                {"detail", "صحيح الرئيسية()"},
                {"documentation", "دالة باء ونقطة بدء البرنامج."},
                {"filter_text", "الرئيسية"},
                {"insert_text", "الرئيسية"},
                {"insert_text_format", "plain"},
                {"scope", "global"}
            },
            {
                {"label", "قيمة_محلية"},
                {"kind", "variable"},
                {"detail", "صحيح قيمة_محلية"},
                {"documentation", "متغير محلي مرئي عند موضع المؤشر."},
                {"filter_text", "قيمة_محلية"},
                {"insert_text", "قيمة_محلية"},
                {"insert_text_format", "plain"},
                {"scope", "local"}
            },
            {
                {"label", "من_واجهة"},
                {"kind", "function"},
                {"detail", "صحيح من_واجهة()"},
                {"documentation", "تصريح مضمّن يملكه مترجم باء."},
                {"filter_text", "من_واجهة"},
                {"insert_text", "من_واجهة"},
                {"insert_text_format", "plain"},
                {"scope", "included"}
            }
        });
        Json symbol = nullptr;
        if (start != std::string::npos) {
            symbol = {
                {"domain", "external"},
                {"kind", "function"},
                {"name", name}
            };
            hover = {
                {"name", name}, {"kind", "function"},
                {"display", "صحيح اجمع(صحيح أول، صحيح ثان)"},
                {"description", "دالة باء"},
                {"range", {
                    {"start", {{"line", 1}, {"column", 6}, {"byte", start}}},
                    {"end", {{"line", 1}, {"column", 10},
                             {"byte", start + name.size()}}}
                }},
                {"declaration", {{"file", "رئيسي.baa"},
                                 {"line", 1}, {"column", 6}}}
            };
            signature = {
                {"name", name},
                {"label", "صحيح اجمع(صحيح أول، صحيح ثان)"},
                {"active_parameter", 1},
                {"variadic", false},
                {"parameters", Json::array({
                    {{"label", "صحيح أول"}},
                    {{"label", "صحيح ثان"}}
                })}
            };
            definition = {
                {"file", "رئيسي.baa"}, {"name", name}, {"kind", "function"},
                {"range", {
                    {"start", {{"line", 1}, {"column", 6}, {"byte", start}}},
                    {"end", {{"line", 1}, {"column", 10},
                             {"byte", start + name.size()}}}
                }}
            };
            Json declaration = definition;
            declaration["role"] = "declaration";
            references.push_back(std::move(declaration));
            const std::size_t use = source.rfind(name);
            if (use != start) {
                references.push_back({
                    {"file", "رئيسي.baa"}, {"name", name}, {"kind", "function"},
                    {"role", "reference"},
                    {"range", {
                        {"start", {{"line", 3}, {"column", 10}, {"byte", use}}},
                        {"end", {{"line", 3}, {"column", 14},
                                 {"byte", use + name.size()}}}
                    }}
                });
            }
        }
        std::cout << Json{
            {"schema_version", "semantic-query-json-v1"},
            {"compiler_version", "test"},
            {"file", "رئيسي.baa"},
            {"position_encoding", "utf-8-bytes"},
            {"position_byte", positionByte},
            {"symbol", symbol},
            {"hover", hover},
            {"signature_help", signature},
            {"definition", definition},
            {"references", references},
            {"completion", {{"items", completionItems}}}
        }.dump();
        return 0;
    }
    if (semanticIndex) {
        const std::string name = "اجمع";
        const std::size_t start = source.find(name);
        Json occurrences = Json::array();
        const Json symbol{
            {"domain", "external"},
            {"kind", "function"},
            {"name", name}
        };
        auto appendOccurrence = [&](std::size_t offset,
                                    std::string_view role,
                                    int line) {
            occurrences.push_back({
                {"symbol", symbol},
                {"role", role},
                {"location", {
                    {"file", logicalFile},
                    {"name", name},
                    {"kind", "function"},
                    {"range", {
                        {"start", {{"line", line}, {"column", 6},
                                   {"byte", offset}}},
                        {"end", {{"line", line}, {"column", 10},
                                 {"byte", offset + name.size()}}}
                    }}
                }}
            });
        };
        if (start != std::string::npos) {
            appendOccurrence(start, "definition", 1);
            const std::size_t use = source.rfind(name);
            if (use != start) appendOccurrence(use, "reference", 3);
        }
        std::cout << Json{
            {"schema_version", "semantic-index-json-v1"},
            {"compiler_version", "test"},
            {"file", logicalFile},
            {"position_encoding", "utf-8-bytes"},
            {"occurrences", occurrences}
        }.dump();
        return 0;
    }
    if (dumpSymbols) {
        if (source.find("انتظر") != std::string::npos) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        const std::string name = "الرئيسية";
        const std::size_t start = source.find(name);
        Json symbols = Json::array();
        if (start != std::string::npos) {
            symbols.push_back({
                {"name", name}, {"kind", "function"}, {"scope", "global"},
                {"span", {
                    {"start", {{"line", 1}, {"column", 10}, {"byte", start}}},
                    {"end", {{"line", 1}, {"column", 26},
                             {"byte", start + name.size()}}}
                }},
                {"return_type", {{"kind", "int"}, {"display", "صحيح"}}},
                {"modifiers", {{"const", false}, {"static", false},
                               {"extern", false}, {"prototype", false}}}
            });
        }
        std::cout << Json{{"schema_version", "symbols-json-v1"},
                          {"compiler_version", "test"},
                          {"file", "رئيسي.baa"},
                          {"position_encoding", "utf-8-bytes"},
                          {"symbols", symbols}}.dump();
        return 0;
    }

    const std::string token = "مفقود";
    const std::size_t start = source.find(token);
    if (dumpTokens) {
        Json tokens = Json::array();
        auto appendToken = [&](std::string_view spelling,
                               std::string_view kind) {
            const std::size_t start = source.find(spelling);
            if (start == std::string::npos) return;
            tokens.push_back({
                {"kind", kind},
                {"span", {
                    {"start", {{"line", 1}, {"column", 1}, {"byte", start}}},
                    {"end", {
                        {"line", 1},
                        {"column", 1 + spelling.size()},
                        {"byte", start + spelling.size()}
                    }}
                }}
            });
        };
        appendToken("صحيح", "type");
        appendToken("١", "number");
        appendToken("// تعليق", "comment");
        std::cout << Json{
            {"schema_version", "tokens-json-v1"},
            {"compiler_version", "test"},
            {"language", "baa"},
            {"file", logicalFile},
            {"position_encoding", "utf-8-bytes"},
            {"source_bytes", source.size()},
            {"tokens", std::move(tokens)}
        }.dump();
        return 0;
    }

    Json diagnostics = Json::array();
    if (start != std::string::npos) {
        diagnostics.push_back({
            {"code", "E100"}, {"severity", "error"}, {"category", "semantic"},
            {"message", "رمز مفقود"},
            {"span", {
                {"start", {{"line", 2}, {"column", 5}, {"byte", start}}},
                {"end", {{"line", 2}, {"column", 10}, {"byte", start + token.size()}}}
            }},
            {"fixes", Json::array({{
                {"id", "E100.insert-int-type"},
                {"title", "عرّف المتغير بإضافة نوعه"},
                {"kind", "quickfix"},
                {"applicability", "safe"},
                {"edits", Json::array({{
                    {"file", logicalFile},
                    {"span", {
                        {"start", {{"line", 2}, {"column", 5}, {"byte", start}}},
                        {"end", {{"line", 2}, {"column", 5}, {"byte", start}}}
                    }},
                    {"new_text", "صحيح "}
                }})}
            }})}
        });
    }
    std::cout << Json{{"schema_version", "diagnostics-json-v1"},
                      {"success", diagnostics.empty()},
                      {"diagnostics", diagnostics}}.dump();
    return diagnostics.empty() ? 0 : 1;
}
