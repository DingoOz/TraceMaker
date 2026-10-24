// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Renders the run report (doc 18 D3, D136) from a route summary (`route --json`, with the sections `route --report`
// adds). The renderer reads nothing but the summary and does no arithmetic: every number and every name it prints is a
// field of the JSON, and the HTML marks each with its JSON pointer (`data-src`), so a reader can trace any figure.
#include <string>

#include <nlohmann/json.hpp>

namespace tmk::report {

enum class Format { Html, Markdown };

// Markdown for a path ending in .md or .markdown, else HTML.
Format format_for_path(const std::string& path);

// A JSON value as the report prints it: integers as they are, other numbers to three decimals without trailing zeros,
// true/false as yes/no, null as n/a, strings as they are.
std::string value_text(const nlohmann::json& v);

// The report of `summary`. `json_name` is the summary file the report points at for its sources (shown as written).
std::string render(const nlohmann::json& summary, Format format, const std::string& json_name);

}  // namespace tmk::report
