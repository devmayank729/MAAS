// Prints a QR code in the terminal using half-block characters (two QR rows
// per text line), so it fits on small laptop screens.
#include "qr.hpp"

#include "qrcodegen.hpp"

namespace maas {

std::string render_qr(const std::string& text, bool ascii_only) {
  qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
  const int border = 2;
  int size = code.getSize();
  auto dark = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= size || y >= size) return false;
    return code.getModule(x, y);
  };
  std::string output;
  if (ascii_only) {
    // Two characters per module; works in any terminal and font.
    for (int y = -border; y < size + border; y++) {
      output += "  ";
      for (int x = -border; x < size + border; x++) output += dark(x, y) ? "  " : "##";
      output += "\n";
    }
    return output;
  }
  bool colour = color_enabled();
  for (int y = -border; y < size + border; y += 2) {
    output += "  ";
    // Force black-on-white so the code scans on dark and light themes alike.
    if (colour) output += "\x1b[30;47m";
    for (int x = -border; x < size + border; x++) {
      bool top = dark(x, y);
      bool bottom = dark(x, y + 1);
      if (colour) {
        if (top && bottom) output += "\xe2\x96\x88";       // full block
        else if (top) output += "\xe2\x96\x80";            // upper half
        else if (bottom) output += "\xe2\x96\x84";         // lower half
        else output += " ";
      } else {
        // No colours: draw the light modules, which looks right on the usual
        // dark terminal background.
        if (!top && !bottom) output += "\xe2\x96\x88";
        else if (!top) output += "\xe2\x96\x80";
        else if (!bottom) output += "\xe2\x96\x84";
        else output += " ";
      }
    }
    if (colour) output += "\x1b[0m";
    output += "\n";
  }
  return output;
}

}  // namespace maas
