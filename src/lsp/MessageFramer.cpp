#include "lsp/MessageFramer.h"

#include <algorithm>
#include <charconv>
#include <cctype>

namespace {
void setError(std::string *target, std::string_view message)
{
    if (target) *target = message;
}

std::string trimmedLower(std::string_view value)
{
    while (not value.empty() and std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (not value.empty() and std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}
}

std::vector<std::string> MessageFramer::appendData(std::string_view data,
                                                   std::string *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    m_buffer.append(data);

    std::vector<std::string> messages;
    while (not m_buffer.empty()) {
        const std::size_t headerEnd = m_buffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            if (m_buffer.size() > MaximumHeaderBytes) {
                setError(errorMessage, "LSP header exceeds the configured limit.");
                clear();
            }
            break;
        }
        if (headerEnd > MaximumHeaderBytes) {
            setError(errorMessage, "LSP header exceeds the configured limit.");
            clear();
            break;
        }

        std::size_t contentLength = std::string::npos;
        std::size_t lineStart = 0;
        while (lineStart < headerEnd) {
            const std::size_t lineEnd = m_buffer.find("\r\n", lineStart);
            const std::size_t boundedEnd = std::min(lineEnd, headerEnd);
            const std::string_view line(m_buffer.data() + lineStart, boundedEnd - lineStart);
            const std::size_t separator = line.find(':');
            if (separator == std::string_view::npos or separator == 0) {
                setError(errorMessage, "Malformed LSP header.");
                clear();
                return messages;
            }

            if (trimmedLower(line.substr(0, separator)) == "content-length") {
                if (contentLength != std::string::npos) {
                    setError(errorMessage, "Duplicate Content-Length header.");
                    clear();
                    return messages;
                }
                std::string raw = trimmedLower(line.substr(separator + 1));
                std::size_t parsed{};
                const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
                if (error != std::errc{} or end != raw.data() + raw.size() or
                    parsed > MaximumContentBytes) {
                    setError(errorMessage, "Invalid Content-Length value.");
                    clear();
                    return messages;
                }
                contentLength = parsed;
            }
            if (lineEnd == std::string::npos or lineEnd >= headerEnd) break;
            lineStart = lineEnd + 2;
        }

        if (contentLength == std::string::npos) {
            setError(errorMessage, "Missing Content-Length header.");
            clear();
            return messages;
        }

        const std::size_t bodyStart = headerEnd + 4;
        if (contentLength > m_buffer.size() - bodyStart) break;
        messages.emplace_back(m_buffer.substr(bodyStart, contentLength));
        m_buffer.erase(0, bodyStart + contentLength);
    }
    return messages;
}

void MessageFramer::clear()
{
    m_buffer.clear();
}

std::string MessageFramer::frame(std::string_view jsonBody)
{
    return "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n\r\n" +
           std::string(jsonBody);
}
