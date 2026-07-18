#include "cli/session.hpp"

#include "script/guile_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cind {

namespace {

BufferId create_session_buffer(EditorRuntime& runtime, std::string initial_text) {
    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> installed = guile.install_core_modes();
    if (!installed) {
        throw std::runtime_error("cannot install session mode policy: " + installed.error());
    }
    const std::optional<ModeId> cpp = runtime.modes().find("cind.cpp");
    if (!cpp) {
        throw std::logic_error("session mode policy did not define cind.cpp");
    }
    const BufferId buffer =
        runtime.buffers().create(BufferSpec{.name = "*session*",
                                            .initial_text = std::move(initial_text),
                                            .kind = BufferKind::Scratch,
                                            .resource_uri = std::nullopt,
                                            .read_only = false});
    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), cpp);
    return buffer;
}

IndentDecision unavailable_indent_decision() {
    return {.target_column = 0,
            .indentation_text = {},
            .role = FormatRole::File,
            .anchor = std::nullopt,
            .preserve = true,
            .trace = {"indentation provider unavailable"}};
}

TextRange editable_range(const Text& text, const SelectionRange& selection) {
    const TextRange ordered = selection.ordered();
    if (ordered.end.value > text.size_bytes()) {
        throw std::out_of_range("selection range is outside the document");
    }
    switch (selection.granularity) {
    case SelectionGranularity::Character:
    case SelectionGranularity::Node:
        return ordered;
    case SelectionGranularity::Line: {
        const std::uint32_t first_line = text.position(ordered.start).line;
        const TextOffset last_position =
            ordered.empty() ? ordered.start : TextOffset{ordered.end.value - 1};
        const std::uint32_t last_line = text.position(last_position).line;
        return TextRange{text.line_start(first_line), text.line_range(last_line).end};
    }
    case SelectionGranularity::Block:
        throw std::invalid_argument("block selection replacement requires rectangular geometry");
    }
    throw std::invalid_argument("unknown selection granularity");
}

struct PlannedSelectionEdit {
    std::size_t selection_index = 0;
    TextRange range;
    std::string_view replacement;
};

struct PlannedCaretInsert {
    TextOffset position;
    std::string_view text;
};

} // namespace

EditSession::EditSession(std::string initial_text, CppIndentStyle style)
    : owned_runtime_(std::make_unique<EditorRuntime>()), runtime_(owned_runtime_.get()),
      buffer_id_(create_session_buffer(*runtime_, std::move(initial_text))),
      view_id_(runtime_->views().create(buffer_id_)),
      style_(std::make_shared<CppIndentStyle>(style)) {}

EditSession::EditSession(EditorRuntime& runtime, BufferId buffer_id, ViewId view_id,
                         CppIndentStyle style)
    : EditSession(runtime, buffer_id, view_id, std::make_shared<CppIndentStyle>(style)) {}

EditSession::EditSession(EditorRuntime& runtime, BufferId buffer_id, ViewId view_id,
                         std::shared_ptr<CppIndentStyle> style)
    : runtime_(&runtime), buffer_id_(buffer_id), view_id_(view_id), style_(std::move(style)) {
    if (!style_) {
        throw std::invalid_argument("EditSession: style must not be null");
    }
    if (runtime_->views().get(view_id_).buffer_id() != buffer_id_) {
        throw std::invalid_argument("EditSession: view does not display the buffer");
    }
}

Document& EditSession::mutable_document() {
    Buffer& target = buffer();
    target.require_writable();
    return target.document_;
}

LanguageMechanismSession& EditSession::language_session(LanguageFacet facet) const {
    const std::optional<LanguageProviderId> provider =
        runtime_->language_provider(buffer_id_, facet);
    if (!provider) {
        throw std::logic_error("language facet has no provider");
    }
    const std::shared_ptr<const LanguageMechanism>& mechanism =
        runtime_->languages().provider(*provider).mechanism;
    const auto existing =
        std::ranges::find_if(language_sessions_, [&](const MechanismState& state) {
            return state.mechanism == mechanism;
        });
    if (existing != language_sessions_.end()) {
        return *existing->session;
    }
    language_sessions_.push_back({.mechanism = mechanism, .session = mechanism->open_session()});
    return *language_sessions_.back().session;
}

void EditSession::apply_language_change(const DocumentChange& change,
                                        const DocumentSnapshot& snapshot,
                                        const LanguageMechanismSession* already_advanced) {
    for (MechanismState& state : language_sessions_) {
        if (state.session.get() != already_advanced) {
            state.session->apply(change, snapshot);
        }
    }
}

const Analysis& EditSession::analysis(LanguageFacet facet) const {
    return language_session(facet).analysis(snapshot());
}

void EditSession::set_caret(TextOffset caret) {
    if (caret.value > snapshot().size_bytes()) {
        throw std::out_of_range("EditSession: caret out of range");
    }
    runtime_->views().set_caret(view_id_, caret);
}

// Ties the undo-tree node the last command created to its caret motion.
void EditSession::record_caret(TextOffset before) {
    undo_carets_[buffer().document_.undo_position()] = CaretPair{before, caret()};
}

void EditSession::type_text(std::string_view text) {
    if (!has_language_facet(LanguageFacet::StructuralEditing)) {
        insert_text(text);
        return;
    }
    // Character by character through the typed-char pipeline, exactly like
    // an editor delivering keystrokes; each character is one undo unit.
    for (char ch : text) {
        const TextOffset before = caret();
        const ViewSelection selection = selection_model();
        std::vector<TextOffset> carets;
        carets.reserve(selection.ranges.size());
        for (const SelectionRange& range : selection.ranges) {
            carets.push_back(range.head);
        }
        LanguageMechanismSession& mechanism = language_session(LanguageFacet::StructuralEditing);
        TypeCharsResult result = mechanism.type_chars(mutable_document(), carets, ch, *style_);
        apply_language_change(result.change, snapshot(), &mechanism);
        record_caret(before);
    }
}

void EditSession::insert_text(std::string_view text) {
    if (text.empty()) {
        return;
    }
    const std::vector<std::string> replacements{std::string(text)};
    insert_text(replacements);
}

void EditSession::insert_text(std::span<const std::string> replacements) {
    const ViewSelection selection = selection_model();
    if (replacements.empty() ||
        (replacements.size() != 1 && replacements.size() != selection.ranges.size())) {
        throw std::invalid_argument(
            "caret insertion requires one text or one text per selection range");
    }

    std::vector<PlannedCaretInsert> inserts;
    inserts.reserve(selection.ranges.size());
    for (std::size_t index = 0; index < selection.ranges.size(); ++index) {
        inserts.push_back(
            {.position = selection.ranges[index].head,
             .text = replacements.size() == 1 ? replacements.front() : replacements[index]});
    }
    std::ranges::sort(inserts, [](const PlannedCaretInsert& left, const PlannedCaretInsert& right) {
        return left.position < right.position;
    });
    std::vector<PlannedCaretInsert> unique;
    unique.reserve(inserts.size());
    for (const PlannedCaretInsert& insert : inserts) {
        if (!unique.empty() && unique.back().position == insert.position) {
            if (unique.back().text != insert.text) {
                throw std::invalid_argument(
                    "coincident selection heads require the same inserted text");
            }
            continue;
        }
        unique.push_back(insert);
    }
    if (std::ranges::none_of(
            unique, [](const PlannedCaretInsert& insert) { return !insert.text.empty(); })) {
        return;
    }

    const TextOffset before = caret();
    EditTransaction tx = mutable_document().begin_transaction();
    for (auto insert = unique.rbegin(); insert != unique.rend(); ++insert) {
        if (!insert->text.empty()) {
            tx.insert(insert->position, insert->text);
        }
    }
    CommitResult commit = tx.commit();
    apply_language_change(commit.change, commit.snapshot);
    record_caret(before);
}

EnterResult EditSession::enter() {
    const TextOffset before = caret();
    if (!has_language_facet(LanguageFacet::Indentation)) {
        EditTransaction transaction = mutable_document().begin_transaction();
        transaction.insert(before, "\n");
        CommitResult commit = transaction.commit();
        apply_language_change(commit.change, commit.snapshot);
        set_caret(TextOffset{before.value + 1});
        record_caret(before);
        return {.handler = "PlainNewline",
                .decision = unavailable_indent_decision(),
                .caret = caret(),
                .change = std::move(commit.change)};
    }
    LanguageMechanismSession& mechanism = language_session(LanguageFacet::Indentation);
    EnterResult result = mechanism.newline(mutable_document(), before, *style_);
    apply_language_change(result.change, snapshot(), &mechanism);
    set_caret(result.caret);
    record_caret(before);
    return result;
}

void EditSession::erase(TextRange range) {
    if (range.empty()) {
        return;
    }
    const TextOffset before = caret();
    EditTransaction tx = mutable_document().begin_transaction();
    tx.erase(range);
    CommitResult commit = tx.commit();
    apply_language_change(commit.change, commit.snapshot);
    set_caret(range.start);
    record_caret(before);
}

ViewSelection EditSession::replace_selection(ViewSelection selection,
                                             std::span<const std::string> replacements) {
    if (selection.ranges.empty()) {
        throw std::invalid_argument("selection replacement requires at least one range");
    }
    if (selection.primary >= selection.ranges.size()) {
        throw std::out_of_range("selection replacement primary range is outside the range list");
    }
    if (replacements.size() != selection.ranges.size()) {
        throw std::invalid_argument("selection replacement requires one text per range");
    }

    const Text content = snapshot().content();
    std::vector<PlannedSelectionEdit> edits;
    edits.reserve(selection.ranges.size());
    for (std::size_t index = 0; index < selection.ranges.size(); ++index) {
        edits.push_back({.selection_index = index,
                         .range = editable_range(content, selection.ranges[index]),
                         .replacement = replacements[index]});
    }
    std::ranges::sort(edits,
                      [](const PlannedSelectionEdit& left, const PlannedSelectionEdit& right) {
                          if (left.range.start != right.range.start) {
                              return left.range.start < right.range.start;
                          }
                          return left.range.end < right.range.end;
                      });
    for (std::size_t index = 1; index < edits.size(); ++index) {
        const PlannedSelectionEdit& previous = edits[index - 1];
        const PlannedSelectionEdit& current = edits[index];
        if (current.range.start < previous.range.end ||
            current.range.start == previous.range.start) {
            throw std::invalid_argument("selection replacement ranges must not overlap");
        }
    }

    ViewSelection result = selection;
    std::int64_t delta = 0;
    for (const PlannedSelectionEdit& edit : edits) {
        const std::int64_t replacement_size = static_cast<std::int64_t>(edit.replacement.size());
        const std::int64_t position =
            static_cast<std::int64_t>(edit.range.start.value) + delta + replacement_size;
        if (position < 0 || position > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("selection replacement exceeds the text offset limit");
        }
        result.ranges[edit.selection_index] = {
            .anchor = TextOffset{static_cast<std::uint32_t>(position)},
            .head = TextOffset{static_cast<std::uint32_t>(position)},
            .granularity = SelectionGranularity::Character};
        delta += replacement_size - static_cast<std::int64_t>(edit.range.length());
    }

    const bool mutates = std::ranges::any_of(edits, [](const PlannedSelectionEdit& edit) {
        return !edit.range.empty() || !edit.replacement.empty();
    });
    if (!mutates) {
        return result;
    }

    const TextOffset before = caret();
    EditTransaction transaction = mutable_document().begin_transaction();
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
        if (!edit->range.empty() || !edit->replacement.empty()) {
            transaction.replace(edit->range, edit->replacement);
        }
    }
    CommitResult commit = transaction.commit();
    apply_language_change(commit.change, commit.snapshot);
    set_caret(result.ranges[result.primary].head);
    record_caret(before);
    return result;
}

std::vector<std::string> EditSession::selection_texts(const ViewSelection& selection) const {
    if (selection.ranges.empty()) {
        throw std::invalid_argument("selection extraction requires at least one range");
    }
    if (selection.primary >= selection.ranges.size()) {
        throw std::out_of_range("selection extraction primary range is outside the range list");
    }
    const Text content = snapshot().content();
    std::vector<std::string> result;
    result.reserve(selection.ranges.size());
    for (const SelectionRange& range : selection.ranges) {
        result.push_back(content.substring(editable_range(content, range)));
    }
    return result;
}

IndentDecision EditSession::indent() {
    if (!has_language_facet(LanguageFacet::Indentation)) {
        return unavailable_indent_decision();
    }
    DocumentSnapshot snap = snapshot();
    const TextOffset caret_before = caret();
    const std::uint32_t line = snap.content().position(caret_before).line;
    const TextOffset line_start = snap.content().line_start(line);
    const std::string content = snap.substring(snap.content().line_content_range(line));
    std::uint32_t old_len = 0;
    while (old_len < content.size() && (content[old_len] == ' ' || content[old_len] == '\t')) {
        ++old_len;
    }
    const bool blank_line = old_len == content.size();

    const TextOffset before = caret_before;
    Document& document = mutable_document();
    const RevisionId revision_before = document.revision();
    IndentDecision decision =
        language_session(LanguageFacet::Indentation).indent_line(document, line, *style_);
    if (document.revision() != revision_before) {
        const auto new_len = static_cast<std::uint32_t>(decision.indentation_text.size());
        if (caret_before.value >= line_start.value + old_len) {
            set_caret(TextOffset{caret_before.value - old_len + new_len});
        } else {
            set_caret(TextOffset{line_start.value + new_len});
        }
        record_caret(before);
    } else if (blank_line && !decision.preserve) {
        const TextOffset target{line_start.value +
                                static_cast<std::uint32_t>(decision.indentation_text.size())};
        if (caret_before != target) {
            set_caret(target);
        }
    }
    return decision;
}

bool EditSession::undo() {
    Buffer& target = buffer();
    const UndoNodeId leaving = target.document_.undo_position();
    std::optional<DocumentChange> change = target.undo();
    if (!change) {
        return false;
    }
    apply_language_change(*change, target.snapshot());
    if (auto it = undo_carets_.find(leaving); it != undo_carets_.end()) {
        set_caret(it->second.before);
    }
    clamp_caret();
    return true;
}

bool EditSession::redo() {
    Buffer& target = buffer();
    std::optional<DocumentChange> change = target.redo();
    if (!change) {
        return false;
    }
    apply_language_change(*change, target.snapshot());
    if (auto it = undo_carets_.find(target.document_.undo_position()); it != undo_carets_.end()) {
        set_caret(it->second.after);
    }
    clamp_caret();
    return true;
}

void EditSession::clamp_caret() {
    TextOffset position = caret();
    position.value = std::min(position.value, snapshot().size_bytes());
    set_caret(position);
}

IndentDecision EditSession::explain() const {
    if (!has_language_facet(LanguageFacet::Indentation)) {
        return unavailable_indent_decision();
    }
    DocumentSnapshot snap = snapshot();
    return language_session(LanguageFacet::Indentation)
        .explain_indent(snap, snap.content().position(caret()).line, *style_);
}

std::string EditSession::render_with_caret() const {
    std::string out = snapshot().content().to_string();
    out.insert(caret().value, "^");
    return out;
}

// key/value order mirrors the textual `key: value` configuration contract.
bool set_style_field(CppIndentStyle& style, std::string_view key, std::string_view value) {
    auto parse_int = [&](int& out) {
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
        return ec == std::errc() && ptr == value.data() + value.size();
    };
    auto parse_bool = [&](bool& out) {
        if (value == "true") {
            out = true;
            return true;
        }
        if (value == "false") {
            out = false;
            return true;
        }
        return false;
    };

    if (key == "indent_width") {
        return parse_int(style.indent_width);
    }
    if (key == "continuation_indent") {
        return parse_int(style.continuation_indent);
    }
    if (key == "tab_width") {
        return parse_int(style.tab_width);
    }
    if (key == "access_specifier_offset") {
        return parse_int(style.access_specifier_offset);
    }
    if (key == "use_tabs") {
        return parse_bool(style.use_tabs);
    }
    if (key == "indent_namespace_body" || key == "namespace_indentation") {
        using NI = CppIndentStyle::NamespaceIndentation;
        if (value == "true" || value == "All") {
            style.namespace_indentation = NI::All;
        } else if (value == "false" || value == "None") {
            style.namespace_indentation = NI::None;
        } else if (value == "Inner") {
            style.namespace_indentation = NI::Inner;
        } else {
            return false;
        }
        return true;
    }
    if (key == "indent_type_body") {
        return parse_bool(style.indent_type_body);
    }
    if (key == "indent_case_label") {
        return parse_bool(style.indent_case_label);
    }
    if (key == "indent_case_body") {
        return parse_bool(style.indent_case_body);
    }
    if (key == "constructor_initializers") {
        using Style = CppIndentStyle::ConstructorInitializerStyle;
        if (value == "NormalIndent") {
            style.constructor_initializers = Style::NormalIndent;
        } else if (value == "ContinuationIndent") {
            style.constructor_initializers = Style::ContinuationIndent;
        } else if (value == "AlignFirstInitializer") {
            style.constructor_initializers = Style::AlignFirstInitializer;
        } else if (value == "AlignWithColon") {
            style.constructor_initializers = Style::AlignWithColon;
        } else if (value == "AlignAfterColon") {
            style.constructor_initializers = Style::AlignAfterColon;
        } else {
            return false;
        }
        return true;
    }
    return false;
}

CaretText split_caret_marker(std::string_view marked) {
    CaretText out;
    std::size_t pos = marked.find('^');
    out.had_marker = pos != std::string_view::npos;
    out.text = std::string(marked);
    if (out.had_marker) {
        out.text.erase(pos, 1);
        out.caret = TextOffset{static_cast<std::uint32_t>(pos)};
    } else {
        out.caret = TextOffset{static_cast<std::uint32_t>(out.text.size())};
    }
    return out;
}

} // namespace cind
