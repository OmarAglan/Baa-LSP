#include "TestHarness.h"
#include "server/BaaLanguageServer.h"

#include <algorithm>
#include <vector>

int main()
{
    BaaLanguageServer server;
    std::vector<Json> messages;
    int requestedExit = -1;
    server.setMessageCallback([&messages](std::string body) {
        messages.push_back(Json::parse(body));
    });
    server.setExitCallback([&requestedExit](int code) { requestedExit = code; });

    server.receiveMessage(R"({"jsonrpc":"2.0","id":1,"method":"shutdown"})");
    CHECK(messages.size() == 1);
    CHECK(messages.back()["error"]["code"] == -32002);

    messages.clear();
    server.receiveMessage(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{}})");
    CHECK(messages.size() == 1);
    CHECK(messages[0]["result"]["capabilities"]["positionEncoding"] == "utf-16");
    CHECK(messages[0]["result"]["capabilities"].contains("textDocumentSync"));
    CHECK(messages[0]["result"]["capabilities"]["documentSymbolProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["foldingRangeProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["selectionRangeProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["semanticTokensProvider"]["full"] ==
          true);
    CHECK(messages[0]["result"]["capabilities"]["semanticTokensProvider"]
              ["legend"]["tokenTypes"] ==
          Json::array({"type", "macro", "keyword", "modifier", "comment",
                       "string", "number", "operator", "function", "variable",
                       "parameter", "property", "enumMember"}));
    CHECK(messages[0]["result"]["capabilities"]["workspaceSymbolProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["workspace"]
              ["workspaceFolders"]["supported"] == true);
    CHECK(messages[0]["result"]["capabilities"]["workspace"]
              ["workspaceFolders"]["changeNotifications"] == true);
    CHECK(messages[0]["result"]["capabilities"]["hoverProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["definitionProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["referencesProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["documentFormattingProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"]["renameProvider"]["prepareProvider"] == true);
    CHECK(messages[0]["result"]["capabilities"].contains("signatureHelpProvider"));
    CHECK(messages[0]["result"]["capabilities"].contains("completionProvider"));
    const Json triggers =
        messages[0]["result"]["capabilities"]["completionProvider"]["triggerCharacters"];
    CHECK(std::ranges::find(triggers, Json("ا")) != triggers.end());
    CHECK(std::ranges::find(triggers, Json("#")) != triggers.end());

    server.receiveMessage(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    server.receiveMessage(R"({"jsonrpc":"2.0","id":3,"method":"shutdown","params":null})");
    CHECK(messages.size() == 2);
    CHECK(messages.back()["result"].is_null());
    server.receiveMessage(R"({"jsonrpc":"2.0","method":"exit","params":null})");
    CHECK(requestedExit == 0);
    return 0;
}
