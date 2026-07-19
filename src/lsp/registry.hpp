#pragma once

#include "editor/ids.hpp"
#include "lsp/session.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cind {

class LspSessionRegistry {
public:
    explicit LspSessionRegistry(AsyncRuntime& runtime) : runtime_(&runtime) {}

    std::expected<LspSessionId, std::string> ensure(std::optional<ProjectId> project,
                                                    LspSessionConfig config);
    LspSession* find(LspSessionId id);
    const LspSession* find(LspSessionId id) const;
    std::vector<LspSessionSnapshot> snapshots() const;
    std::size_t close_document(std::string_view uri);
    bool erase(LspSessionId id);

private:
    struct Entry {
        std::optional<ProjectId> project;
        std::unique_ptr<LspSession> session;
    };

    AsyncRuntime* runtime_;
    std::uint64_t next_id_ = 0;
    std::vector<Entry> sessions_;
};

} // namespace cind
