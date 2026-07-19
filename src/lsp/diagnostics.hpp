#pragma once

#include "editor/diagnostic.hpp"
#include "lsp/protocol.hpp"
#include "lsp/session.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cind {

struct LspDiagnostic {
    LspRange range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
    std::string source;
    std::string code;
};

struct LspPublishedDiagnostics {
    std::string uri;
    std::optional<RevisionId> version;
    std::vector<LspDiagnostic> diagnostics;
};

class LspDiagnosticsFeature {
public:
    using Published = std::function<void(LspPublishedDiagnostics)>;
    using Failed = std::function<void(std::string)>;

    static void attach(LspSession& session, Published published, Failed failed);
    static std::string client_capabilities();
};

} // namespace cind
