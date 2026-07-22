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
    void cancel();

private:
    std::mutex m_mutex;
    std::uintptr_t m_processToken{};
    bool m_cancelRequested{};
};
