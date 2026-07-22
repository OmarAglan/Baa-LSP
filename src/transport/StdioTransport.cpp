#include "transport/StdioTransport.h"

#include <array>
#include <cstdio>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

void StdioTransport::setMessageCallback(MessageCallback callback)
{
    m_messageCallback = std::move(callback);
}

void StdioTransport::setErrorCallback(ErrorCallback callback)
{
    m_errorCallback = std::move(callback);
}

void StdioTransport::run()
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::array<char, 64 * 1024> buffer{};
    while (not m_stopRequested.load()) {
#if defined(_WIN32)
        const int count = _read(_fileno(stdin), buffer.data(), static_cast<unsigned>(buffer.size()));
#else
        const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
#endif
        if (count <= 0) return;

        std::string error;
        const std::vector<std::string> messages = m_framer.appendData(
            std::string_view(buffer.data(), static_cast<std::size_t>(count)), &error);
        if (not error.empty() and m_errorCallback) m_errorCallback(error);
        for (const std::string &message : messages) {
            if (m_messageCallback) m_messageCallback(message);
            if (m_stopRequested.load()) return;
        }
    }
}

void StdioTransport::requestStop()
{
    m_stopRequested.store(true);
}

void StdioTransport::writeMessage(std::string_view jsonBody)
{
    const std::string framed = MessageFramer::frame(jsonBody);
    std::scoped_lock lock(m_outputMutex);
    const std::size_t written = std::fwrite(framed.data(), 1, framed.size(), stdout);
    std::fflush(stdout);
    if (written != framed.size() and m_errorCallback) {
        m_errorCallback("Failed to write an LSP message to stdout.");
    }
}
