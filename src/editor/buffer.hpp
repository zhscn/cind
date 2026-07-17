#pragma once

#include "document/document.hpp"
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
class ProjectRegistry;
class ViewRegistry;

enum class BufferKind : std::uint8_t {
    File,
    Scratch,
    Generated,
    Process,
    Minibuffer,
};

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
    LinePosition target;

    friend bool operator==(const BufferLocation&, const BufferLocation&) = default;
};

class Buffer {
public:
    BufferId id() const { return id_; }
    DocumentId document_id() const { return document_.id(); }
    const std::string& name() const { return name_; }
    BufferKind kind() const { return kind_; }
    const std::optional<std::string>& resource_uri() const { return resource_uri_; }
    std::optional<ProjectId> project_id() const { return project_id_; }

    bool read_only() const { return read_only_; }
    void set_read_only(bool read_only) { read_only_ = read_only; }
    bool modified() const;
    const Text& save_point() const { return save_point_; }
    void mark_saved(Text content) { save_point_ = std::move(content); }

    DocumentSnapshot snapshot() const { return document_.snapshot(); }
    EditTransaction begin_transaction();
    std::optional<DocumentChange> undo();
    std::optional<DocumentChange> redo();

    SettingsLayer& settings() { return settings_; }
    const SettingsLayer& settings() const { return settings_; }
    BufferModes& modes() { return modes_; }
    const BufferModes& modes() const { return modes_; }
    std::vector<KeymapId>& keymaps() { return keymaps_; }
    const std::vector<KeymapId>& keymaps() const { return keymaps_; }
    std::uint32_t attached_view_count() const { return attached_views_; }
    const std::vector<BufferLocation>& locations() const;
    const BufferLocation* location_at(TextOffset offset) const;

private:
    friend class BufferRegistry;
    friend class EditSession;
    friend class ProjectRegistry;
    friend class ViewRegistry;

    Buffer(BufferId id, DocumentId document_id, BufferSpec spec, const SettingRegistry& settings,
           ModeRegistry& modes);
    void require_writable() const;

    BufferId id_;
    std::string name_;
    BufferKind kind_;
    std::optional<std::string> resource_uri_;
    std::optional<ProjectId> project_id_;
    bool read_only_ = false;
    Document document_;
    Text save_point_;
    SettingsLayer settings_;
    BufferModes modes_;
    std::vector<KeymapId> keymaps_;
    std::vector<BufferLocation> locations_;
    RevisionId locations_revision_ = 0;
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

    std::optional<BufferId> find_by_name(std::string_view name) const;
    std::optional<BufferId> find_by_resource(std::string_view uri) const;
    std::vector<BufferId> all() const;

    void rename(BufferId id, std::string requested_name);
    void set_resource(BufferId id, std::optional<std::string> uri, BufferKind kind);
    void set_locations(BufferId id, std::vector<BufferLocation> locations);

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<Buffer> value;
    };

    std::string unique_name(std::string requested,
                            std::optional<BufferId> self = std::nullopt) const;
    static std::string fallback_name(const BufferSpec& spec);

    const SettingRegistry* settings_;
    ModeRegistry* modes_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<std::string, BufferId> by_name_;
    std::unordered_map<std::string, BufferId> by_resource_;
    DocumentId next_document_id_ = 1;
};

} // namespace cind
