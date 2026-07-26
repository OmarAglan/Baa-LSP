#include "TestHarness.h"
#include "lsp/PositionEncoding.h"

int main()
{
    const std::string text = "س = ١.\nص = ٢.\n";
    const std::size_t secondLine = text.find("ص");
    Json position = PositionEncoding::utf16PositionForByteOffset(text, secondLine);
    CHECK(position["line"] == 1);
    CHECK(position["character"] == 0);

    const std::string emoji = "😀س";
    position = PositionEncoding::utf16PositionForByteOffset(emoji, std::string("😀").size());
    CHECK(position["line"] == 0);
    CHECK(position["character"] == 2);
    CHECK(PositionEncoding::utf8ByteOffsetForUtf16Position(text, 1, 1) ==
          secondLine + std::string("ص").size());
    CHECK(PositionEncoding::utf8ByteOffsetForUtf16Position(emoji, 0, 2) ==
          std::string("😀").size());
    CHECK(PositionEncoding::utf8ByteOffsetForUtf16Position(emoji, 0, 1) == 0);
    CHECK(PositionEncoding::utf8ByteOffsetForUtf16Position(text, 99, 0) == text.size());

    const std::string source = "صحيح الرئيسية() {\n    مفقود = ١.\n}\n";
    const std::size_t start = source.find("مفقود");
    const std::size_t end = start + std::string("مفقود").size();
    const std::size_t insertion = source.find("}");
    const Json diagnostics = Json::array({{
        {"code", "E100"}, {"severity", "error"}, {"message", "رمز مفقود"},
        {"span", {{"start", {{"byte", start}}}, {"end", {{"byte", end}}}}},
        {"fixes", Json::array({{
            {"id", "B0001.insert-dot"},
            {"title", "أضف '.'"},
            {"kind", "quickfix"},
            {"applicability", "safe"},
            {"edits", Json::array({{
                {"file", "رئيسي.baa"},
                {"span", {
                    {"start", {{"byte", insertion}}},
                    {"end", {{"byte", insertion}}}
                }},
                {"new_text", "."}
            }})}
        }})}
    }});
    const Json converted = PositionEncoding::baaDiagnosticsToLsp(source, diagnostics);
    CHECK(converted.size() == 1);
    CHECK(converted[0]["source"] == "باء");
    CHECK(converted[0]["severity"] == 1);
    CHECK(converted[0]["range"]["start"]["line"] == 1);
    CHECK(converted[0]["range"]["start"]["character"] == 4);
    CHECK(converted[0]["data"]["fixes"].size() == 1);
    CHECK(converted[0]["data"]["fixes"][0]["title"] == "أضف '.'");
    CHECK(converted[0]["data"]["fixes"][0]["edits"][0]["range"]["start"] ==
          converted[0]["data"]["fixes"][0]["edits"][0]["range"]["end"]);
    CHECK(converted[0]["data"]["fixes"][0]["edits"][0]["range"]["start"]["line"] == 2);
    CHECK(converted[0]["data"]["fixes"][0]["edits"][0]["newText"] == ".");

    Json invalidBoundaryDiagnostics = diagnostics;
    invalidBoundaryDiagnostics[0]["fixes"][0]["edits"][0]["span"]["start"]["byte"] =
        start + 1;
    invalidBoundaryDiagnostics[0]["fixes"][0]["edits"][0]["span"]["end"]["byte"] =
        start + 1;
    const Json invalidBoundaryConverted =
        PositionEncoding::baaDiagnosticsToLsp(source, invalidBoundaryDiagnostics);
    CHECK(not invalidBoundaryConverted[0].contains("data") or
          not invalidBoundaryConverted[0]["data"].contains("fixes"));

    const std::string symbolSource = "صحيح اجمع(صحيح أ، صحيح ب) {}\n";
    const std::size_t functionStart = symbolSource.find("اجمع");
    const std::size_t functionEnd = functionStart + std::string("اجمع").size();
    const std::size_t parameterStart = symbolSource.find("أ");
    const std::size_t parameterEnd = parameterStart + std::string("أ").size();
    const Json symbols = Json::array({{
        {"name", "اجمع"}, {"kind", "function"},
        {"span", {
            {"start", {{"byte", functionStart}}},
            {"end", {{"byte", functionEnd}}}
        }},
        {"return_type", {{"kind", "int"}, {"display", "صحيح"}}},
        {"children", Json::array({{
            {"name", "أ"}, {"kind", "parameter"},
            {"span", {
                {"start", {{"byte", parameterStart}}},
                {"end", {{"byte", parameterEnd}}}
            }},
            {"type", {{"kind", "int"}, {"display", "صحيح"}}}
        }})}
    }});
    const Json convertedSymbols = PositionEncoding::baaSymbolsToLsp(symbolSource, symbols);
    CHECK(convertedSymbols.size() == 1);
    CHECK(convertedSymbols[0]["name"] == "اجمع");
    CHECK(convertedSymbols[0]["kind"] == 12);
    CHECK(convertedSymbols[0]["detail"] == "-> صحيح");
    CHECK(convertedSymbols[0]["selectionRange"]["start"]["character"] == 5);
    CHECK(convertedSymbols[0]["selectionRange"]["end"]["character"] == 9);
    CHECK(convertedSymbols[0]["children"][0]["name"] == "أ");
    CHECK(convertedSymbols[0]["children"][0]["kind"] == 13);
    CHECK(convertedSymbols[0]["children"][0]["range"]["start"]["character"] == 15);

    const Json hover = {
        {"display", "صحيح اجمع(صحيح أول، صحيح ثان)"},
        {"description", "دالة باء"},
        {"range", {
            {"start", {{"byte", functionStart}}},
            {"end", {{"byte", functionEnd}}}
        }}
    };
    const Json convertedHover =
        PositionEncoding::baaSemanticHoverToLsp(symbolSource, hover);
    CHECK(convertedHover["contents"]["kind"] == "markdown");
    CHECK(convertedHover["contents"]["value"].get<std::string>().find("دالة باء") !=
          std::string::npos);
    CHECK(convertedHover["range"]["start"]["character"] == 5);
    CHECK(convertedHover["range"]["end"]["character"] == 9);

    const Json signature = {
        {"label", "صحيح اجمع(صحيح أول، صحيح ثان)"},
        {"active_parameter", 1},
        {"parameters", Json::array({
            {{"label", "صحيح أول"}}, {{"label", "صحيح ثان"}}
        })}
    };
    const Json convertedSignature =
        PositionEncoding::baaSignatureHelpToLsp(signature);
    CHECK(convertedSignature["activeSignature"] == 0);
    CHECK(convertedSignature["activeParameter"] == 1);
    CHECK(convertedSignature["signatures"][0]["parameters"].size() == 2);

    const Json location = {
        {"file", "مسار/رئيسي.baa"},
        {"range", {
            {"start", {{"line", 1}, {"column", 6}, {"byte", functionStart}}},
            {"end", {{"line", 1}, {"column", 10}, {"byte", functionEnd}}}
        }}
    };
    const Json convertedLocation = PositionEncoding::baaLocationToLsp(
        symbolSource, "file:///مسار/%D8%B1%D8%A6%D9%8A%D8%B3%D9%8A.baa",
        location);
    CHECK(convertedLocation["range"]["start"]["character"] == 5);
    CHECK(convertedLocation["range"]["end"]["character"] == 9);

    Json includedLocation = location;
    includedLocation["range"]["start"].erase("byte");
    includedLocation["range"]["end"].erase("byte");
    includedLocation["range"]["start"]["column"] =
        static_cast<int>(functionStart + 1);
    includedLocation["range"]["end"]["column"] =
        static_cast<int>(functionEnd + 1);
    const Json convertedIncluded = PositionEncoding::baaLocationToLsp(
        symbolSource, "file:///مسار/%D9%88%D8%A7%D8%AC%D9%87%D8%A9.baahd",
        includedLocation);
    CHECK(convertedIncluded["range"]["start"]["character"] == 5);
    CHECK(convertedIncluded["range"]["end"]["character"] == 9);
    return 0;
}
