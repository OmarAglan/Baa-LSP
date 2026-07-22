#include "process/ProcessRunner.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
#if defined(_WIN32)
std::wstring wideFromUtf8(std::string_view text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring quoteWindowsArgument(std::wstring_view argument)
{
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(character);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring commandLine(const std::string &program,
                         const std::vector<std::string> &arguments)
{
    std::wstring result = quoteWindowsArgument(wideFromUtf8(program));
    for (const std::string &argument : arguments) {
        result.push_back(L' ');
        result += quoteWindowsArgument(wideFromUtf8(argument));
    }
    return result;
}

void readWindowsPipe(HANDLE pipe, std::string &output)
{
    std::array<char, 16 * 1024> buffer{};
    DWORD count{};
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) and
           count > 0) {
        output.append(buffer.data(), count);
    }
    CloseHandle(pipe);
}

void writeWindowsPipe(HANDLE pipe, std::string_view input)
{
    std::size_t offset = 0;
    while (offset < input.size()) {
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(input.size() - offset,
                                                                      64 * 1024));
        DWORD written{};
        if (not WriteFile(pipe, input.data() + offset, wanted, &written, nullptr) or written == 0) {
            break;
        }
        offset += written;
    }
    CloseHandle(pipe);
}
#else
void readPosixPipe(int descriptor, std::string &output)
{
    std::array<char, 16 * 1024> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(descriptor);
}

void writePosixPipe(int descriptor, std::string_view input)
{
    std::size_t offset = 0;
    while (offset < input.size()) {
        const ssize_t written = ::write(descriptor, input.data() + offset, input.size() - offset);
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    ::close(descriptor);
}
#endif
}

void ProcessRunner::prepare()
{
    std::scoped_lock lock(m_mutex);
    m_cancelRequested = false;
    m_processToken = 0;
}

ProcessResult ProcessRunner::run(const std::string &program,
                                 const std::vector<std::string> &arguments,
                                 const std::filesystem::path &workingDirectory,
                                 std::string_view standardInput)
{
    ProcessResult result;
    const auto finishState = [this, &result] {
        std::scoped_lock lock(m_mutex);
        result.cancelled = m_cancelRequested;
        m_cancelRequested = false;
        m_processToken = 0;
    };

#if defined(_WIN32)
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inputRead = nullptr;
    HANDLE inputWrite = nullptr;
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    HANDLE errorRead = nullptr;
    HANDLE errorWrite = nullptr;
    auto closeIfValid = [](HANDLE handle) {
        if (handle and handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    };

    if (not CreatePipe(&inputRead, &inputWrite, &security, 0) or
        not CreatePipe(&outputRead, &outputWrite, &security, 0) or
        not CreatePipe(&errorRead, &errorWrite, &security, 0)) {
        result.errorMessage = "Failed to create compiler process pipes.";
        closeIfValid(inputRead); closeIfValid(inputWrite);
        closeIfValid(outputRead); closeIfValid(outputWrite);
        closeIfValid(errorRead); closeIfValid(errorWrite);
        finishState();
        return result;
    }
    SetHandleInformation(inputWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errorRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inputRead;
    startup.hStdOutput = outputWrite;
    startup.hStdError = errorWrite;
    PROCESS_INFORMATION process{};

    std::wstring command = commandLine(program, arguments);
    std::wstring directory = workingDirectory.empty() ? std::wstring{} : workingDirectory.wstring();
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr,
                                        directory.empty() ? nullptr : directory.c_str(),
                                        &startup, &process);
    closeIfValid(inputRead);
    closeIfValid(outputWrite);
    closeIfValid(errorWrite);
    if (not started) {
        result.errorMessage = "Failed to start Baa compiler (Windows error " +
                              std::to_string(GetLastError()) + ").";
        closeIfValid(inputWrite); closeIfValid(outputRead); closeIfValid(errorRead);
        finishState();
        return result;
    }

    result.started = true;
    {
        std::scoped_lock lock(m_mutex);
        m_processToken = reinterpret_cast<std::uintptr_t>(process.hProcess);
        if (m_cancelRequested) TerminateProcess(process.hProcess, 125);
    }

    std::thread inputThread(writeWindowsPipe, inputWrite, standardInput);
    std::thread outputThread(readWindowsPipe, outputRead, std::ref(result.standardOutput));
    std::thread errorThread(readWindowsPipe, errorRead, std::ref(result.standardError));
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode{};
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    inputThread.join();
    outputThread.join();
    errorThread.join();

    finishState();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    int inputPipe[2]{};
    int outputPipe[2]{};
    int errorPipe[2]{};
    if (::pipe(inputPipe) != 0 or ::pipe(outputPipe) != 0 or ::pipe(errorPipe) != 0) {
        result.errorMessage = std::string("Failed to create compiler process pipes: ") +
                              std::strerror(errno);
        finishState();
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.errorMessage = std::string("Failed to fork Baa compiler: ") + std::strerror(errno);
        for (const int descriptor : {inputPipe[0], inputPipe[1], outputPipe[0], outputPipe[1],
                                     errorPipe[0], errorPipe[1]}) ::close(descriptor);
        finishState();
        return result;
    }
    if (pid == 0) {
        ::dup2(inputPipe[0], STDIN_FILENO);
        ::dup2(outputPipe[1], STDOUT_FILENO);
        ::dup2(errorPipe[1], STDERR_FILENO);
        for (const int descriptor : {inputPipe[0], inputPipe[1], outputPipe[0], outputPipe[1],
                                     errorPipe[0], errorPipe[1]}) ::close(descriptor);
        if (not workingDirectory.empty()) ::chdir(workingDirectory.c_str());

        std::vector<char *> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const std::string &argument : arguments) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(program.c_str(), argv.data());
        _exit(127);
    }

    ::close(inputPipe[0]);
    ::close(outputPipe[1]);
    ::close(errorPipe[1]);
    result.started = true;
    {
        std::scoped_lock lock(m_mutex);
        m_processToken = static_cast<std::uintptr_t>(pid);
        if (m_cancelRequested) ::kill(pid, SIGKILL);
    }

    std::thread inputThread(writePosixPipe, inputPipe[1], standardInput);
    std::thread outputThread(readPosixPipe, outputPipe[0], std::ref(result.standardOutput));
    std::thread errorThread(readPosixPipe, errorPipe[0], std::ref(result.standardError));
    int status{};
    ::waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                    : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    inputThread.join();
    outputThread.join();
    errorThread.join();

    finishState();
#endif
    return result;
}

void ProcessRunner::cancel()
{
    std::scoped_lock lock(m_mutex);
    m_cancelRequested = true;
    if (m_processToken == 0) return;
#if defined(_WIN32)
    TerminateProcess(reinterpret_cast<HANDLE>(m_processToken), 125);
#else
    ::kill(static_cast<pid_t>(m_processToken), SIGKILL);
#endif
}
