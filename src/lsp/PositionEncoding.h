#pragma once

#include "lsp/Json.h"

#include <cstddef>
#include <string_view>

namespace PositionEncoding {

Json utf16PositionForByteOffset(std::string_view text, std::size_t utf8ByteOffset);
std::size_t utf8ByteOffsetForUtf16Position(std::string_view text,
                                           int zeroBasedLine,
                                           int utf16Character);
std::size_t byteOffsetForOneBasedLocation(std::string_view text, int line, int column);
Json baaDiagnosticsToLsp(std::string_view text, const Json &diagnostics);
Json baaSymbolsToLsp(std::string_view text, const Json &symbols);
Json baaTokensToLspData(std::string_view text, const Json &tokens);
Json baaFoldingRangesToLsp(std::string_view text, const Json &ranges);
Json baaSelectionRangesToLsp(std::string_view text,
                             const Json &ranges,
                             const Json &positions);
Json baaSemanticHoverToLsp(std::string_view text, const Json &hover);
Json baaSignatureHelpToLsp(const Json &signatureHelp);
Json baaLocationToLsp(std::string_view text,
                      std::string_view uri,
                      const Json &location);

}
