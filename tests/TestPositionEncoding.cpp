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

    const std::string source = "صحيح الرئيسية() {\n    مفقود = ١.\n}\n";
    const std::size_t start = source.find("مفقود");
    const std::size_t end = start + std::string("مفقود").size();
    const Json diagnostics = Json::array({{
        {"code", "E100"}, {"severity", "error"}, {"message", "رمز مفقود"},
        {"span", {{"start", {{"byte", start}}}, {"end", {{"byte", end}}}}}
    }});
    const Json converted = PositionEncoding::baaDiagnosticsToLsp(source, diagnostics);
    CHECK(converted.size() == 1);
    CHECK(converted[0]["source"] == "باء");
    CHECK(converted[0]["severity"] == 1);
    CHECK(converted[0]["range"]["start"]["line"] == 1);
    CHECK(converted[0]["range"]["start"]["character"] == 4);
    return 0;
}
