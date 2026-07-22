#include "lsp/Json.h"

#include <iostream>
#include <iterator>
#include <string>

int main()
{
    const std::string source((std::istreambuf_iterator<char>(std::cin)),
                             std::istreambuf_iterator<char>());
    const std::string token = "مفقود";
    const std::size_t start = source.find(token);
    Json diagnostics = Json::array();
    if (start != std::string::npos) {
        diagnostics.push_back({
            {"code", "E100"}, {"severity", "error"}, {"category", "semantic"},
            {"message", "رمز مفقود"},
            {"span", {
                {"start", {{"line", 2}, {"column", 5}, {"byte", start}}},
                {"end", {{"line", 2}, {"column", 10}, {"byte", start + token.size()}}}
            }}
        });
    }
    std::cout << Json{{"schema_version", "diagnostics-json-v1"},
                      {"success", diagnostics.empty()},
                      {"diagnostics", diagnostics}}.dump();
    return diagnostics.empty() ? 0 : 1;
}
