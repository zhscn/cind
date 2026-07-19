#pragma once

#include "lsp/protocol.hpp"
#include "lsp/session.hpp"

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cind {

struct LspWorkspaceDocumentEdit {
    std::string resource;
    std::optional<RevisionId> version;
    std::vector<LspTextEdit> edits;
};

struct LspWorkspaceEdit {
    std::vector<LspWorkspaceDocumentEdit> documents;
    bool has_resource_operations = false;
};

struct LspCommand {
    std::string title;
    std::string command;
    std::string arguments = "[]";
};

struct LspCodeAction {
    std::string title;
    std::string kind;
    bool preferred = false;
    std::string disabled_reason;
    std::optional<LspWorkspaceEdit> edit;
    std::optional<LspCommand> command;
    bool resolved = true;
    std::string raw;
};

struct LspRefactorRequest {
    std::string uri;
    std::string language_id;
    RevisionId revision = 0;
    Text text;
    TextOffset caret;
    TextRange range;
};

class LspRefactorFeature {
public:
    using RenameCompleted = std::function<void(std::optional<LspWorkspaceEdit>)>;
    using CodeActionsCompleted = std::function<void(std::vector<LspCodeAction>)>;
    using CodeActionResolved = std::function<void(LspCodeAction)>;
    using Failed = std::function<void(std::string)>;
    using Cancelled = std::function<void()>;
    using Cancel = LspSession::Cancel;

    static std::expected<Cancel, std::string>
    rename(LspSession& session, const LspRefactorRequest& request, const std::string& new_name,
           RenameCompleted completed, Failed failed, Cancelled cancelled = {});
    static std::expected<Cancel, std::string>
    code_actions(LspSession& session, const LspRefactorRequest& request,
                 CodeActionsCompleted completed, Failed failed, Cancelled cancelled = {});
    static std::expected<Cancel, std::string>
    resolve_code_action(LspSession& session, const std::string& raw, CodeActionResolved completed,
                        Failed failed, Cancelled cancelled = {});

    static bool supports_rename(const LspSession& session);
    static bool supports_code_actions(const LspSession& session);
    static bool supports_code_action_resolve(const LspSession& session);
    static std::string client_capabilities();
};

} // namespace cind
