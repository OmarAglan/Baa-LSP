#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct ProcessResult
{
    bool started{};
    bool cancelled{};
    int exitCode{-1};
    std::string standardOutput;
    std::string standardError;
    std::string errorMessage;
};

class ProcessRunner
{
public:
    void prepare();
    ProcessResult run(const std::string &program,
                      const std::vector<std::string> &arguments,
                      const std::filesystem::path &workingDirectory,
                      std::string_view standardInput);
    // Runs the request through Baa's linked frontend (baa-frontend-api-v1)
    // instead of a new process. The result has the same exit code, stdout and
    // stderr that the executable would produce. A call cannot be interrupted;
    // cancel() marks its result cancelled when it returns.
    ProcessResult runInProcess(const std::string &program,
                               const std::vector<std::string> &arguments,
                               const std::filesystem::path &workingDirectory,
                               std::string_view standardInput);
    void cancel();

    static bool inProcessFrontendAvailable();

private:
    std::mutex m_mutex;
    std::uintptr_t m_processToken{};
    bool m_cancelRequested{};
};
