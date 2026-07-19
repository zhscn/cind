#include "lsp/registry.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace cind {

std::expected<LspSessionId, std::string>
LspSessionRegistry::ensure(std::optional<ProjectId> project, LspSessionConfig config) {
    const auto same_owner = [&](const Entry& entry) {
        return entry.project == project && entry.session->config().root == config.root &&
               entry.session->config().language_id == config.language_id;
    };
    if (const auto found = std::ranges::find_if(sessions_, same_owner); found != sessions_.end()) {
        const LspSessionConfig& existing = found->session->config();
        if (existing.command == config.command && existing.arguments == config.arguments) {
            return found->session->id();
        }
        sessions_.erase(found);
    }
    try {
        const LspSessionId id{++next_id_};
        sessions_.push_back(
            {.project = project,
             .session = std::make_unique<LspSession>(id, *runtime_, std::move(config))});
        return id;
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

LspSession* LspSessionRegistry::find(LspSessionId id) {
    const auto found = std::ranges::find_if(
        sessions_, [id](const Entry& entry) { return entry.session->id() == id; });
    return found == sessions_.end() ? nullptr : found->session.get();
}

const LspSession* LspSessionRegistry::find(LspSessionId id) const {
    return const_cast<LspSessionRegistry*>(this)->find(id);
}

std::vector<LspSessionSnapshot> LspSessionRegistry::snapshots() const {
    std::vector<LspSessionSnapshot> result;
    result.reserve(sessions_.size());
    for (const Entry& entry : sessions_) {
        result.push_back(entry.session->snapshot());
    }
    return result;
}

std::size_t LspSessionRegistry::close_document(std::string_view uri) {
    std::size_t closed = 0;
    for (Entry& entry : sessions_) {
        closed += entry.session->close_document(uri) ? 1 : 0;
    }
    return closed;
}

bool LspSessionRegistry::erase(LspSessionId id) {
    const auto found = std::ranges::find_if(
        sessions_, [id](const Entry& entry) { return entry.session->id() == id; });
    if (found == sessions_.end()) {
        return false;
    }
    sessions_.erase(found);
    return true;
}

} // namespace cind
