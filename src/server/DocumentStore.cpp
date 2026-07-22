#include "server/DocumentStore.h"

namespace {
void setError(std::string *target, std::string_view message)
{
    if (target) *target = message;
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
    auto it = m_documents.find(uri);
    if (it == m_documents.end()) {
        setError(errorMessage, "Document is not open in the language server.");
        return false;
    }
    if (version <= it->second.version) {
        setError(errorMessage, "Document version is stale.");
        return false;
    }
    it->second.version = version;
    it->second.text = text;
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

void DocumentStore::clear()
{
    m_documents.clear();
}
