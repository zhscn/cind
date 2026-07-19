#pragma once

#include "lsp/protocol.hpp"
#include "lsp/session.hpp"

#include <expected>
#include <functional>
#include <string>

namespace cind {

// Method-specific adapter layered on the generic LspSession request API.
// It owns completion JSON and exposes normalized protocol values to editor code.
class LspCompletionFeature {
public:
    using Completed = std::function<void(LspCompletionResponse)>;
    using ResolveCompleted = std::function<void(LspCompletionItem)>;
    using Failed = std::function<void(std::string)>;
    using Cancelled = std::function<void()>;
    using Cancel = LspSession::Cancel;

    static std::expected<Cancel, std::string> request(LspSession& session,
                                                      LspCompletionRequest request,
                                                      Completed completed, Failed failed,
                                                      Cancelled cancelled);
    static std::expected<Cancel, std::string> resolve(LspSession& session, const std::string& item,
                                                      ResolveCompleted completed, Failed failed,
                                                      Cancelled cancelled);
    static bool supports_resolve(const LspSession& session);
    static std::string client_capabilities();
};

} // namespace cind
