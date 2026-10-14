#include "server/DocumentStore.h"

#include "lsp/PositionEncoding.h"

#include <algorithm>
#include <string_view>

namespace {
void setError(std::string *target, std::string_view message)
{
    if (target) *target = message;
}

// Resolves an LSP UTF-16 position to a byte offset. A character beyond the end
// of its line means the line end, as LSP specifies. A negative value, a line
// past the last line, or a character inside a surrogate pair is rejected:
// guessing would apply the edit somewhere the client did not mean.
bool byteOffsetForPosition(std::string_view text, DocumentPosition position,
                           std::size_t &offset)
{
    if (position.line < 0 or position.character < 0) return false;
    std::size_t lineStart = 0;
    for (int line = 0; line < position.line; ++line) {
        const std::size_t newline = text.find('\n', lineStart);
        if (newline == std::string_view::npos) return false;
        lineStart = newline + 1;
    }
    const std::size_t newline = text.find('\n', lineStart);
    const std::string_view lineText = text.substr(
        lineStart,
        newline == std::string_view::npos ? std::string_view::npos : newline - lineStart);
    const std::size_t within =
        PositionEncoding::utf8ByteOffsetForUtf16Position(lineText, 0, position.character);
    if (within < lineText.size() and
        PositionEncoding::utf16PositionForByteOffset(lineText, within)
                .value("character", -1) != position.character) {
        return false;
    }
    offset = lineStart + within;
    return true;
}
}

bool DocumentStore::open(const BaaDocument &document, std::string *errorMessage)
{
    if (document.uri.empty()) {
        setError(errorMessage, "Document URI is missing.");
        return false;
    }
    if (document.languageId != "baa") {
        setError(errorMessage, "Baa-LSP accepts Baa documents only.");
        return false;
    }
    m_documents.insert_or_assign(document.uri, document);
    return true;
}

bool DocumentStore::change(const std::string &uri, int version, const std::string &text,
                           std::string *errorMessage)
{
    return applyChanges(uri, version, {DocumentChange{false, {}, {}, text}}, errorMessage);
}

bool DocumentStore::applyChanges(const std::string &uri, int version,
                                 const std::vector<DocumentChange> &changes,
                                 std::string *errorMessage)
{
    auto it = m_documents.find(uri);
    if (it == m_documents.end()) {
        setError(errorMessage, "Document is not open in the language server.");
        return false;
    }
    BaaDocument &document = it->second;
    if (changes.empty()) {
        setError(errorMessage, "Document change is empty.");
        return false;
    }
    const bool incremental = std::ranges::any_of(
        changes, [](const DocumentChange &change) { return change.hasRange; });
    if (version <= document.version) {
        // A ranged edit for a version already seen means the edits arrived out
        // of order, so later ranges would land on the wrong text.
        if (incremental) document.synchronized = false;
        setError(errorMessage, "Document version is stale.");
        return false;
    }

    std::string text = document.text;
    bool known = document.synchronized;
    for (const DocumentChange &change : changes) {
        if (not change.hasRange) {
            text = change.text;
            known = true;
            continue;
        }
        if (not known) {
            setError(errorMessage,
                     "Document is out of sync; only a full-text change is accepted.");
            return false;
        }
        std::size_t start{};
        std::size_t end{};
        if (not byteOffsetForPosition(text, change.start, start) or
            not byteOffsetForPosition(text, change.end, end) or start > end) {
            document.synchronized = false;
            setError(errorMessage, "Document change range is not inside the document.");
            return false;
        }
        text.replace(start, end - start, change.text);
    }
    document.text = std::move(text);
    document.version = version;
    document.synchronized = true;
    return true;
}

bool DocumentStore::close(const std::string &uri)
{
    return m_documents.erase(uri) > 0;
}

bool DocumentStore::contains(const std::string &uri) const
{
    return m_documents.contains(uri);
}

BaaDocument DocumentStore::document(const std::string &uri) const
{
    const auto it = m_documents.find(uri);
    return it == m_documents.end() ? BaaDocument{} : it->second;
}

std::vector<BaaDocument> DocumentStore::documents() const
{
    std::vector<BaaDocument> result;
    result.reserve(m_documents.size());
    for (const auto &[uri, document] : m_documents) {
        (void)uri;
        result.push_back(document);
    }
    return result;
}

void DocumentStore::clear()
{
    m_documents.clear();
}
