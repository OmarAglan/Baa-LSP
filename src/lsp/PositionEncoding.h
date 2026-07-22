#pragma once

#include "lsp/Json.h"

#include <cstddef>
#include <string_view>

namespace PositionEncoding {

Json utf16PositionForByteOffset(std::string_view text, std::size_t utf8ByteOffset);
std::size_t byteOffsetForOneBasedLocation(std::string_view text, int line, int column);
Json baaDiagnosticsToLsp(std::string_view text, const Json &diagnostics);

}
