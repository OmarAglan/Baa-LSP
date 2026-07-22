#include "TestHarness.h"
#include "lsp/MessageFramer.h"

int main()
{
    const std::string body = R"({"رسالة":"باء"})";
    const std::string framed = MessageFramer::frame(body);
    CHECK(framed.starts_with("Content-Length: " + std::to_string(body.size())));

    MessageFramer framer;
    std::string error;
    const auto partial = framer.appendData(framed.substr(0, 9), &error);
    CHECK(partial.empty());
    CHECK(error.empty());
    const auto complete = framer.appendData(framed.substr(9), &error);
    CHECK(error.empty());
    CHECK(complete.size() == 1);
    CHECK(complete.front() == body);

    const std::string first = R"({"jsonrpc":"2.0","id":1})";
    const std::string second = R"({"jsonrpc":"2.0","id":2})";
    const auto multiple = framer.appendData(MessageFramer::frame(first) +
                                             MessageFramer::frame(second), &error);
    CHECK(multiple.size() == 2);
    CHECK(multiple[0] == first and multiple[1] == second);

    const auto malformed = framer.appendData("Content-Length: nope\r\n\r\n{}", &error);
    CHECK(malformed.empty());
    CHECK(not error.empty());
    return 0;
}
