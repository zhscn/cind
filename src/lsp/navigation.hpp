#pragma once

#include "lsp/protocol.hpp"
#include "lsp/session.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

enum class LspNavigationKind : std::uint8_t {
    Definition,
    Declaration,
    Implementation,
    References,
};

struct LspNavigationRequest {
    std::string uri;
    std::string language_id;
    RevisionId revision = 0;
    Text text;
    TextOffset caret;
    bool include_declaration = true;
};

struct LspLocation {
    std::string resource;
    LspRange range;
};

std::string_view lsp_navigation_name(LspNavigationKind kind);

class LspNavigationFeature {
public:
    using Completed = std::function<void(std::vector<LspLocation>)>;
    using Failed = std::function<void(std::string)>;
    using Cancelled = std::function<void()>;
    using Cancel = LspSession::Cancel;

    static std::expected<Cancel, std::string> request(LspSession& session, LspNavigationKind kind,
                                                      LspNavigationRequest request,
                                                      Completed completed, Failed failed,
                                                      Cancelled cancelled = {});
    static bool supported(const LspSession& session, LspNavigationKind kind);
    static std::string client_capabilities();
};

} // namespace cind
