#include "compiler/BaaCompilerBridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
std::string trimmed(std::string value)
{
    const auto isSpace = [](unsigned char character) { return std::isspace(character); };
    value.erase(value.begin(), std::ranges::find_if_not(value, isSpace));
    value.erase(std::ranges::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
                value.end());
    return value;
}

std::filesystem::path pathFromUtf8(std::string_view value)
{
    const auto *first = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path(std::u8string(first, first + value.size()));
}

std::string pathToUtf8(const std::filesystem::path &path)
{
    const std::u8string encoded = path.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}
}

BaaCompilerBridge::BaaCompilerBridge()
    : m_worker(&BaaCompilerBridge::workerLoop, this)
{
}

BaaCompilerBridge::~BaaCompilerBridge()
{
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
        m_pending.clear();
        m_latestVersions.clear();
        m_callback = {};
    }
    m_runner.cancel();
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void BaaCompilerBridge::setCompilerProgram(std::string program)
{
    std::scoped_lock lock(m_mutex);
    m_compilerProgram = trimmed(std::move(program));
}

void BaaCompilerBridge::setApplicationDirectory(std::filesystem::path directory)
{
    std::scoped_lock lock(m_mutex);
    m_applicationDirectory = std::move(directory);
}

void BaaCompilerBridge::setDebounceInterval(int milliseconds)
{
    std::scoped_lock lock(m_mutex);
    m_debounce = std::chrono::milliseconds(std::max(0, milliseconds));
}

void BaaCompilerBridge::setAnalysisCallback(AnalysisCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_callback = std::move(callback);
}

void BaaCompilerBridge::schedule(BaaAnalysisRequest request)
{
    if (not request.isValid()) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.insert_or_assign(request.uri, request.version);
        m_pending.insert_or_assign(request.uri, request);
        cancelActive = m_activeUri == request.uri;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancel(const std::string &uri)
{
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        m_pending.erase(uri);
        m_latestVersions.erase(uri);
        cancelActive = m_activeUri == uri;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelAll()
{
    bool hasActive = false;
    {
        std::scoped_lock lock(m_mutex);
        m_pending.clear();
        m_latestVersions.clear();
        hasActive = not m_activeUri.empty();
        ++m_scheduleSerial;
    }
    if (hasActive) m_runner.cancel();
    m_wake.notify_one();
}

std::string BaaCompilerBridge::resolveCompilerProgram() const
{
    std::string configured;
    std::filesystem::path applicationDirectory;
    {
        std::scoped_lock lock(m_mutex);
        configured = m_compilerProgram;
        applicationDirectory = m_applicationDirectory;
    }
    if (configured.empty()) {
        if (const char *environment = std::getenv("BAA")) configured = trimmed(environment);
    }
    if (not configured.empty()) return configured;

#if defined(_WIN32)
    constexpr std::string_view executable = "baa.exe";
#else
    constexpr std::string_view executable = "baa";
#endif
    if (not applicationDirectory.empty()) {
        const std::filesystem::path nested = applicationDirectory / "baa" / executable;
        if (std::filesystem::is_regular_file(nested)) return pathToUtf8(nested);
        const std::filesystem::path adjacent = applicationDirectory / executable;
        if (std::filesystem::is_regular_file(adjacent)) return pathToUtf8(adjacent);
    }
    return std::string(executable);
}

void BaaCompilerBridge::workerLoop()
{
    while (true) {
        BaaAnalysisRequest request;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping or not m_pending.empty(); });
            if (m_stopping) return;

            const std::uint64_t observedSerial = m_scheduleSerial;
            const std::chrono::milliseconds debounce = m_debounce;
            if (m_wake.wait_for(lock, debounce, [this, observedSerial] {
                    return m_stopping or m_scheduleSerial != observedSerial;
                })) {
                if (m_stopping) return;
                continue;
            }
            if (m_pending.empty()) continue;

            auto next = m_pending.begin();
            request = std::move(next->second);
            m_pending.erase(next);
            m_runner.prepare();
            m_activeUri = request.uri;
        }

        const std::string compiler = resolveCompilerProgram();
        const std::filesystem::path sourcePath = pathFromUtf8(request.filePath);
        ProcessResult process = m_runner.run(
            compiler,
            {"--check", "--diagnostics=json", "--source-stdin=" + request.filePath},
            sourcePath.parent_path(), request.text);

        BaaAnalysisResult result;
        result.uri = request.uri;
        result.filePath = request.filePath;
        result.text = request.text;
        result.version = request.version;
        result.exitCode = process.exitCode;

        if (not process.started) {
            result.errorMessage = process.errorMessage.empty()
                ? "Baa compiler executable was not found."
                : process.errorMessage;
        } else if (not process.cancelled) {
            const Json parsed = Json::parse(process.standardOutput, nullptr, false);
            if (not parsed.is_discarded() and parsed.is_object() and
                parsed.value("schema_version", "") == "diagnostics-json-v1" and
                parsed.value("diagnostics", Json::array()).is_array()) {
                result.diagnostics = parsed["diagnostics"];
            } else {
                result.errorMessage = "Baa returned diagnostics that do not satisfy diagnostics-json-v1.";
                if (not process.standardError.empty()) {
                    result.errorMessage += " " + trimmed(process.standardError);
                }
            }
        }

        AnalysisCallback callback;
        bool publish = false;
        {
            std::scoped_lock lock(m_mutex);
            m_activeUri.clear();
            const auto latest = m_latestVersions.find(request.uri);
            publish = not process.cancelled and latest != m_latestVersions.end() and
                      latest->second == request.version;
            callback = m_callback;
        }
        if (publish and callback) callback(std::move(result));
        m_wake.notify_one();
    }
}
