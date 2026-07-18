#pragma once

#include "cli/session.hpp"
#include "formatting/format_role.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace cind {

using EditSessionResolver = std::function<EditSession&(ViewId)>;

enum class DeleteGraphemeMode : std::uint8_t {
    Raw,
    Structural,
};

enum class DeleteGraphemeOutcome : std::uint8_t {
    Unchanged,
    Deleted,
    MovedOverPair,
    MovedOverLiteral,
};

struct EditingMechanismHooks {
    std::function<void()> edited;
    std::function<void()> caret_moved;
};

// Atomic editor capabilities used by policy runtimes. This type owns no
// command names, key bindings, messages, repeat semantics, or selection
// policy. Its operations preserve the invariants of EditSession and View.
class EditingMechanisms {
public:
    EditingMechanisms(EditSessionResolver session, EditingMechanismHooks hooks);

    bool undo(ViewId view);
    bool redo(ViewId view);
    void reset_preferred_column(ViewId view);
    void move_lines(ViewId view, std::int64_t delta);
    void move_line_boundary(ViewId view, bool end);
    DeleteGraphemeOutcome delete_grapheme(ViewId view, bool forward,
                                          DeleteGraphemeMode mode);
    void newline(ViewId view);
    std::optional<FormatRole> indent(ViewId view);
    void type_text(ViewId view, std::string_view text);

private:
    DeleteGraphemeOutcome structural_delete(ViewId view, bool forward);
    DeleteGraphemeOutcome raw_delete(ViewId view, bool forward);
    void notify_edited();
    void notify_caret_moved();

    EditSessionResolver session_;
    EditingMechanismHooks hooks_;
};

} // namespace cind
