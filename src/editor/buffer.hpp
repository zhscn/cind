#pragma once

#include "document/document.hpp"
#include "editor/diagnostic.hpp"
#include "editor/ids.hpp"
#include "editor/mode.hpp"
#include "editor/settings.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

class EditSession;
class ViewRegistry;

enum class BufferKind : std::uint8_t {
    File,
    Scratch,
    Generated,
    Process,
    Minibuffer,
};

// What the host is asked to create. Name, kind and resource are all policy and
// live in Guile; they are forwarded to it at creation and not stored here.
struct BufferSpec {
    std::string name;
    std::string initial_text;
    BufferKind kind = BufferKind::Scratch;
    std::optional<std::string> resource_uri;
    bool read_only = false;
};

// A semantic link from a range in a generated buffer to a source location.
// Location-list, compilation and diagnostic buffers share this contract;
// the payload remains frontend- and mode-independent.
struct BufferLocation {
    TextRange source_range;
    std::string resource;
    EncodedLinePosition target;
    std::string excerpt;

    friend bool operator==(const BufferLocation&, const BufferLocation&) = default;
};

class Buffer {
public:
    BufferId id() const { return id_; }
    DocumentId document_id() const { return document_.id(); }

    bool read_only() const { return read_only_; }
    void set_read_only(bool read_only) { read_only_ = read_only; }
    bool modified() const;
    const Text& save_point() const { return save_point_; }
    void mark_saved(Text content);
    std::uint32_t save_generation() const { return save_generation_; }

    DocumentSnapshot snapshot() const { return document_.snapshot(); }
    EditTransaction begin_transaction();
    std::optional<DocumentChange> undo();
    std::optional<DocumentChange> redo();
    UndoNodeId undo_position() const { return document_.undo_position(); }
    DocumentChange undo_to(UndoNodeId position);
    AnchorId create_navigation_anchor(TextOffset offset,
                                      AnchorAffinity affinity = AnchorAffinity::AfterInsertion);
    void remove_navigation_anchor(AnchorId anchor);
    TextOffset navigation_anchor_offset(AnchorId anchor) const;
    std::optional<TextOffset> editable_start() const { return document_.editable_start(); }
    void set_editable_start(std::optional<TextOffset> offset) {
        document_.set_editable_start(offset);
    }

    SettingsLayer& settings() { return settings_; }
    const SettingsLayer& settings() const { return settings_; }
    BufferModes& modes() { return modes_; }
    const BufferModes& modes() const { return modes_; }
    std::vector<KeymapId>& keymaps() { return keymaps_; }
    const std::vector<KeymapId>& keymaps() const { return keymaps_; }
    std::uint32_t attached_view_count() const { return attached_views_; }
    const std::vector<BufferLocation>& locations() const;
    const BufferLocation* location_at(TextOffset offset) const;
    std::vector<Diagnostic> diagnostics() const;
    std::uint64_t diagnostics_generation() const { return diagnostics_generation_; }

private:
    friend class BufferRegistry;
    friend class EditSession;
    friend class ViewRegistry;

    Buffer(BufferId id, DocumentId document_id, BufferSpec spec, const SettingRegistry& settings,
           ModeRegistry& modes);
    void require_writable() const;

    BufferId id_;
    bool read_only_ = false;
    Document document_;
    Text save_point_;
    std::uint32_t save_generation_ = 0;
    SettingsLayer settings_;
    BufferModes modes_;
    std::vector<KeymapId> keymaps_;
    std::vector<BufferLocation> locations_;
    RevisionId locations_revision_ = 0;
    std::unordered_map<std::string, DiagnosticSet> diagnostic_sets_;
    std::uint64_t diagnostics_generation_ = 0;
    std::uint32_t attached_views_ = 0;
};

class BufferRegistry {
public:
    BufferRegistry(const SettingRegistry& settings, ModeRegistry& modes)
        : settings_(&settings), modes_(&modes) {}

    BufferId create(BufferSpec spec);
    bool erase(BufferId id);

    Buffer& get(BufferId id);
    const Buffer& get(BufferId id) const;
    Buffer* try_get(BufferId id);
    const Buffer* try_get(BufferId id) const;

    std::vector<BufferId> all() const;

    void set_locations(BufferId id, std::vector<BufferLocation> locations);
    void set_diagnostics(BufferId id, std::string owner, RevisionId revision,
                         std::vector<Diagnostic> diagnostics);
    bool clear_diagnostics(BufferId id, std::string_view owner);

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<Buffer> value;
    };

    const SettingRegistry* settings_;
    ModeRegistry* modes_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    DocumentId next_document_id_ = 1;
};

} // namespace cind
