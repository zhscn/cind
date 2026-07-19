#include "editor/workbench_session.hpp"

#include <cmath>
#include <format>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace cind {

namespace {

constexpr std::size_t maximum_collection_size = 100'000;

void write_layout(std::ostream& output, const WorkbenchLayoutSessionState& node) {
    if (node.window) {
        const WorkbenchWindowSessionState& window = *node.window;
        output << "leaf " << window.resource.has_value() << ' '
               << std::quoted(window.resource.value_or(std::string{})) << ' ' << window.caret << ' '
               << window.role.has_value() << ' ' << std::quoted(window.role.value_or(std::string{}))
               << ' ' << window.pinned << ' ' << window.created_by_policy << ' '
               << window.jump_walk.size() << ' ' << window.jump_cursor.has_value() << ' '
               << window.jump_cursor.value_or(0);
        for (const std::uint64_t jump : window.jump_walk) {
            output << ' ' << jump;
        }
        output << '\n';
        return;
    }
    if (!node.first || !node.second || !std::isfinite(node.ratio) || node.ratio <= 0.0F ||
        node.ratio >= 1.0F) {
        throw std::invalid_argument("workbench session contains an invalid layout branch");
    }
    output << "branch " << (node.axis == WindowSplitAxis::Rows ? "rows" : "columns") << ' '
           << node.ratio << '\n';
    write_layout(output, *node.first);
    write_layout(output, *node.second);
}

std::size_t read_size(std::istream& input, const char* field) {
    std::uint64_t value = 0;
    if (!(input >> value) || value > maximum_collection_size) {
        throw std::invalid_argument(std::format("invalid {}", field));
    }
    return static_cast<std::size_t>(value);
}

bool read_bool(std::istream& input, const char* field) {
    int value = -1;
    if (!(input >> value) || (value != 0 && value != 1)) {
        throw std::invalid_argument(std::format("invalid {}", field));
    }
    return value != 0;
}

std::uint64_t read_u64(std::istream& input, const char* field) {
    std::uint64_t value = 0;
    if (!(input >> value)) {
        throw std::invalid_argument(std::format("invalid {}", field));
    }
    return value;
}

std::string read_quoted(std::istream& input, const char* field) {
    std::string value;
    if (!(input >> std::quoted(value))) {
        throw std::invalid_argument(std::format("invalid {}", field));
    }
    return value;
}

WorkbenchLayoutSessionState read_layout(std::istream& input, std::size_t& nodes) {
    if (++nodes > maximum_collection_size) {
        throw std::invalid_argument("workbench layout is too large");
    }
    std::string kind;
    if (!(input >> kind)) {
        throw std::invalid_argument("missing workbench layout node");
    }
    if (kind == "leaf") {
        const bool has_resource = read_bool(input, "leaf resource marker");
        std::string resource = read_quoted(input, "leaf resource");
        std::uint64_t caret = 0;
        if (!(input >> caret) || caret > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("invalid leaf caret");
        }
        const bool has_role = read_bool(input, "leaf role marker");
        std::string role = read_quoted(input, "leaf role");
        const bool pinned = read_bool(input, "leaf pinned state");
        const bool created = read_bool(input, "leaf policy provenance");
        const std::size_t walk_count = read_size(input, "leaf jump walk count");
        const bool has_jump_cursor = read_bool(input, "leaf jump cursor marker");
        const std::size_t jump_cursor = read_size(input, "leaf jump cursor");
        std::vector<std::uint64_t> jump_walk;
        jump_walk.reserve(walk_count);
        for (std::size_t index = 0; index < walk_count; ++index) {
            jump_walk.push_back(read_u64(input, "leaf jump node"));
        }
        if ((has_resource && resource.empty()) || (has_role && role.empty())) {
            throw std::invalid_argument("workbench leaf contains an empty stable identifier");
        }
        if ((has_jump_cursor && jump_cursor >= jump_walk.size()) ||
            (!has_jump_cursor && !jump_walk.empty())) {
            throw std::invalid_argument("workbench leaf contains an invalid jump cursor");
        }
        return {
            .window =
                WorkbenchWindowSessionState{
                    .resource = has_resource ? std::optional(std::move(resource)) : std::nullopt,
                    .caret = static_cast<std::uint32_t>(caret),
                    .role = has_role ? std::optional(std::move(role)) : std::nullopt,
                    .pinned = pinned,
                    .created_by_policy = created,
                    .jump_walk = std::move(jump_walk),
                    .jump_cursor = has_jump_cursor ? std::optional(jump_cursor) : std::nullopt},
            .axis = WindowSplitAxis::Rows,
            .ratio = 0.5F,
            .first = nullptr,
            .second = nullptr};
    }
    if (kind != "branch") {
        throw std::invalid_argument("unknown workbench layout node");
    }
    std::string axis;
    float ratio = 0.0F;
    if (!(input >> axis >> ratio) || (axis != "rows" && axis != "columns") ||
        !std::isfinite(ratio) || ratio <= 0.0F || ratio >= 1.0F) {
        throw std::invalid_argument("invalid workbench layout branch");
    }
    auto first = std::make_unique<WorkbenchLayoutSessionState>(read_layout(input, nodes));
    auto second = std::make_unique<WorkbenchLayoutSessionState>(read_layout(input, nodes));
    return {.window = std::nullopt,
            .axis = axis == "rows" ? WindowSplitAxis::Rows : WindowSplitAxis::Columns,
            .ratio = ratio,
            .first = std::move(first),
            .second = std::move(second)};
}

} // namespace

std::string serialize_workbench_session(const WorkbenchSessionState& state) {
    if (state.version != WorkbenchSessionState::current_version || state.workbenches.empty() ||
        state.active_workbench >= state.workbenches.size()) {
        throw std::invalid_argument("workbench session header is invalid");
    }
    std::ostringstream output;
    output << "cind-workbench-session " << state.version << ' ' << state.active_workbench << ' '
           << state.workbenches.size() << '\n';
    for (const WorkbenchSessionEntry& workbench : state.workbenches) {
        output << "workbench " << std::quoted(workbench.name) << ' '
               << workbench.scope_roots.size();
        for (const std::string& root : workbench.scope_roots) {
            output << ' ' << std::quoted(root);
        }
        output << ' ' << workbench.mru_resources.size();
        for (const std::string& resource : workbench.mru_resources) {
            output << ' ' << std::quoted(resource);
        }
        output << ' ' << workbench.active_leaf << '\n';
        write_layout(output, workbench.layout);
        output << "jump-nodes " << workbench.jump_nodes.size() << '\n';
        for (const WorkbenchJumpNodeSessionState& node : workbench.jump_nodes) {
            output << "jump-node " << node.id << ' ' << std::quoted(node.resource) << ' '
                   << node.fallback.line << ' ' << node.fallback.byte_column << ' '
                   << std::quoted(node.excerpt) << ' ' << node.created_at << ' ' << node.last_visit
                   << '\n';
        }
        output << "jump-edges " << workbench.jump_edges.size() << '\n';
        for (const WorkbenchJumpEdgeSessionState& edge : workbench.jump_edges) {
            output << "jump-edge " << edge.from << ' ' << edge.to << ' '
                   << std::quoted(edge.kind) << ' ' << edge.at << ' ' << edge.persistent << '\n';
        }
    }
    return output.str();
}

std::expected<WorkbenchSessionState, std::string>
parse_workbench_session(std::string_view serialized) {
    try {
        std::istringstream input{std::string(serialized)};
        std::string magic;
        std::uint64_t version = 0;
        if (!(input >> magic >> version) || magic != "cind-workbench-session" ||
            version != WorkbenchSessionState::current_version) {
            return std::unexpected("unsupported workbench session format");
        }
        WorkbenchSessionState state;
        state.version = static_cast<std::uint32_t>(version);
        state.active_workbench = read_size(input, "active workbench index");
        const std::size_t count = read_size(input, "workbench count");
        if (count == 0 || state.active_workbench >= count) {
            return std::unexpected("workbench session has no valid active workbench");
        }
        state.workbenches.reserve(count);
        std::size_t nodes = 0;
        for (std::size_t index = 0; index < count; ++index) {
            std::string marker;
            if (!(input >> marker) || marker != "workbench") {
                return std::unexpected("missing workbench session entry");
            }
            WorkbenchSessionEntry workbench;
            workbench.name = read_quoted(input, "workbench name");
            const std::size_t scope_count = read_size(input, "scope root count");
            workbench.scope_roots.reserve(scope_count);
            for (std::size_t root = 0; root < scope_count; ++root) {
                workbench.scope_roots.push_back(read_quoted(input, "scope root"));
            }
            const std::size_t mru_count = read_size(input, "MRU resource count");
            workbench.mru_resources.reserve(mru_count);
            for (std::size_t resource = 0; resource < mru_count; ++resource) {
                workbench.mru_resources.push_back(read_quoted(input, "MRU resource"));
            }
            workbench.active_leaf = read_size(input, "active leaf index");
            workbench.layout = read_layout(input, nodes);
            if (!(input >> marker) || marker != "jump-nodes") {
                return std::unexpected("missing workbench jump nodes");
            }
            const std::size_t jump_node_count = read_size(input, "jump node count");
            workbench.jump_nodes.reserve(jump_node_count);
            for (std::size_t node = 0; node < jump_node_count; ++node) {
                if (!(input >> marker) || marker != "jump-node") {
                    return std::unexpected("missing workbench jump node");
                }
                WorkbenchJumpNodeSessionState jump;
                jump.id = read_u64(input, "jump node id");
                jump.resource = read_quoted(input, "jump node resource");
                const std::uint64_t line = read_u64(input, "jump node line");
                const std::uint64_t column = read_u64(input, "jump node column");
                if (line > std::numeric_limits<std::uint32_t>::max() ||
                    column > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected("invalid workbench jump position");
                }
                jump.fallback = {.line = static_cast<std::uint32_t>(line),
                                 .byte_column = static_cast<std::uint32_t>(column)};
                jump.excerpt = read_quoted(input, "jump node excerpt");
                jump.created_at = read_u64(input, "jump node creation time");
                jump.last_visit = read_u64(input, "jump node visit time");
                workbench.jump_nodes.push_back(std::move(jump));
            }
            if (!(input >> marker) || marker != "jump-edges") {
                return std::unexpected("missing workbench jump edges");
            }
            const std::size_t jump_edge_count = read_size(input, "jump edge count");
            workbench.jump_edges.reserve(jump_edge_count);
            for (std::size_t edge = 0; edge < jump_edge_count; ++edge) {
                if (!(input >> marker) || marker != "jump-edge") {
                    return std::unexpected("missing workbench jump edge");
                }
                WorkbenchJumpEdgeSessionState jump;
                jump.from = read_u64(input, "jump edge origin");
                jump.to = read_u64(input, "jump edge target");
                jump.kind = read_quoted(input, "jump edge kind");
                jump.at = read_u64(input, "jump edge time");
                jump.persistent = read_bool(input, "jump edge persistence");
                workbench.jump_edges.push_back(std::move(jump));
            }
            state.workbenches.push_back(std::move(workbench));
        }
        input >> std::ws;
        if (!input.eof()) {
            return std::unexpected("workbench session contains trailing data");
        }
        return state;
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

} // namespace cind
