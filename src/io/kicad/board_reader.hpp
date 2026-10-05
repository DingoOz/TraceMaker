// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Reads .kicad_pcb files (KiCad 9 and 10 formats) into the board model, keeping the s-expression document
// for lossless writing.
#include <string>

#include "model/board.hpp"
#include "sexpr/sexpr.hpp"

namespace tmk::io {

struct LoadedBoard {
  sexpr::Document doc;
  model::Board board;
};

// Throws sexpr::ParseError or std::runtime_error on malformed files.
LoadedBoard read_board_file(const std::string& path);
// Reads the board as the document would be written: pending edits are applied first (the returned board's node
// ids then refer to the re-parsed text, so do not use them to edit `doc` further).
model::Board read_board(const sexpr::Document& doc);

}  // namespace tmk::io
