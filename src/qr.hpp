#pragma once

#include "common.hpp"

namespace maas {

// Returns the QR code for `text` as printable lines.
std::string render_qr(const std::string& text, bool ascii_only = false);

}  // namespace maas
