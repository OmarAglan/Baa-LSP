#include "server/BaaLanguageServer.h"
#include "transport/StdioTransport.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
int runServer(const std::vector<std::string> &arguments,
              const std::filesystem::path &executablePath)
{
    std::string compilerProgram;
    std::string takweenProgram;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string &argument = arguments[index];
        if (argument == "--version") {
            std::cerr << "Baa-LSP 0.1.0\n";
            return 0;
        }
        if (argument == "--baa-path" and index + 1 < arguments.size()) {
            compilerProgram = arguments[++index];
        } else if (argument.starts_with("--baa-path=")) {
            compilerProgram = argument.substr(std::string("--baa-path=").size());
        } else if (argument == "--takween-path" and index + 1 < arguments.size()) {
            takweenProgram = arguments[++index];
        } else if (argument.starts_with("--takween-path=")) {
            takweenProgram =
                argument.substr(std::string("--takween-path=").size());
        }
    }

    StdioTransport transport;
    BaaLanguageServer server;
    server.setCompilerProgram(std::move(compilerProgram));
    server.setTakweenProgram(std::move(takweenProgram));
    server.setApplicationDirectory(std::filesystem::absolute(executablePath).parent_path());

    std::atomic_int exitCode{0};
    transport.setMessageCallback(
        [&server](std::string message) { server.receiveMessage(message); });
    transport.setErrorCallback(
        [](std::string message) { std::cerr << message << '\n'; });
    server.setMessageCallback(
        [&transport](std::string message) { transport.writeMessage(message); });
    server.setExitCallback([&transport, &exitCode](int code) {
        exitCode.store(code);
        transport.requestStop();
    });

    transport.run();
    return exitCode.load();
}

#if defined(_WIN32)
std::string utf8FromWide(std::wstring_view text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}
#endif
}

#if defined(_WIN32)
int main()
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (not argv or argc == 0) return 2;
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) arguments.push_back(utf8FromWide(argv[index]));
    const std::filesystem::path executablePath(argv[0]);
    LocalFree(argv);
    return runServer(arguments, executablePath);
}
#else
int main(int argc, char *argv[])
{
    std::vector<std::string> arguments(argv, argv + argc);
    return runServer(arguments, std::filesystem::path(argv[0]));
}
#endif
