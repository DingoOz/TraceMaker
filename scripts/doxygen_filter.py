#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Doxygen input filter (D134): reads the project's plain `//` comments as documentation.

The headers document every declaration with ordinary `//` comments above it (and `// ...` after members). Doxygen
only reads `///`, so instead of rewriting every header this filter converts, for Doxygen's eyes only:

  * the comment block right after `#pragma once`        -> the file's description (`/// @file`);
  * a `//` block directly above a declaration           -> `///` (a block followed by a blank line is a section
                                                           heading and stays a plain comment);
  * `code;  // text` or `Value,  // text` at line end   -> `///< text`.

Line numbers are kept, so Doxygen's "defined at line N" stays right. Usage (Doxyfile): INPUT_FILTER = "python3
scripts/doxygen_filter.py"; it also runs by hand: `scripts/doxygen_filter.py src/route/obstacles.hpp`.
"""
import re
import sys

COMMENT = re.compile(r"^(\s*)//(?!/)\s?(.*)$")
NOT_DOCUMENTABLE = re.compile(r"^\s*(#|}|$|//|namespace\b|public:|private:|protected:)")


def escape(text: str) -> str:
    # The comments are plain text: `<name>` is a placeholder and `@REF` a word, not HTML or Doxygen commands.
    parts = text.split("`")
    for i in range(0, len(parts), 2):  # outside code spans
        parts[i] = re.sub(r"([<>@&\\])", r"\\\1", parts[i])
    return "`".join(parts)


def trailing(line: str) -> str:
    # `code; // text` -> `code; ///< text`, only after a declaration's end and outside string literals.
    m = re.match(r"^(.*?[;,{])(\s*)//(?![/<])\s?(.*)$", line)
    if not m or m.group(1).count('"') % 2:
        return line
    return f"{m.group(1)}{m.group(2)}///< {escape(m.group(3))}"


def convert(lines: list[str]) -> list[str]:
    out = list(lines)
    i = 0
    while i < len(lines):
        if lines[i].strip() == "#pragma once" and i + 1 < len(lines) and COMMENT.match(lines[i + 1]):
            out[i] = "/// @file"
            j = i + 1
            while j < len(lines) and COMMENT.match(lines[j]):
                out[j] = "/// " + escape(COMMENT.match(lines[j]).group(2))
                j += 1
            i = j
            continue
        m = COMMENT.match(lines[i])
        if m and not lines[i].lstrip().startswith("// SPDX"):
            j = i
            while j < len(lines) and COMMENT.match(lines[j]):
                j += 1
            if j < len(lines) and not NOT_DOCUMENTABLE.match(lines[j]):
                for k in range(i, j):
                    c = COMMENT.match(lines[k])
                    out[k] = f"{c.group(1)}/// {escape(c.group(2))}"
            i = j
            continue
        out[i] = trailing(lines[i])
        i += 1
    return out


def main() -> int:
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()
    sys.stdout.write("\n".join(convert(text.split("\n"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
