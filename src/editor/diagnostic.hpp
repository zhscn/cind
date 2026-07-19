#pragma once

#include "document/text_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cind {

enum class DiagnosticSeverity : std::uint8_t {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

struct Diagnostic {
    TextRange range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
    std::string source;
    std::string code;

    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

struct DiagnosticSet {
    std::string owner;
    RevisionId revision = 0;
    std::vector<Diagnostic> diagnostics;
};

} // namespace cind
