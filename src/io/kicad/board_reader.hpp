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
model::Board read_board(const sexpr::Document& doc);

}  // namespace tmk::io
