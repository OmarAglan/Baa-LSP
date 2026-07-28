#include "lsp/Json.h"

#include <chrono>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

int main(int argc, char **argv)
{
    const std::string source((std::istreambuf_iterator<char>(std::cin)),
                             std::istreambuf_iterator<char>());
    bool dumpSymbols = false;
    bool dumpTokens = false;
    bool completionData = false;
    bool formatJson = false;
    bool semanticQuery = false;
    bool semanticIndex = false;
    std::string logicalFile = "رئيسي.baa";
    std::size_t positionByte = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--dump-symbols=json") {
            dumpSymbols = true;
        } else if (argument == "--dump-tokens=json") {
            dumpTokens = true;
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
        }
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
