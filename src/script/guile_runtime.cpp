#include "script/guile_runtime.hpp"

#include "editor/runtime.hpp"

#include <libguile.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace cind {

namespace {

struct ScriptCommand {
    SCM execute = SCM_UNDEFINED;
    SCM enabled = SCM_BOOL_F;
};

struct GuileState {
    std::thread::id owner;
    bool active = true;
    std::vector<ScriptCommand> commands;
    std::uint64_t command_revision = 0;
    std::optional<std::string> last_error;
};

struct HostLease {
    EditorRuntime* runtime = nullptr;
    std::shared_ptr<GuileState> state;
    GuileHostServices services;
    std::size_t commands_installed = 0;
    std::size_t bindings_installed = 0;
};

SCM host_type = SCM_UNDEFINED;
std::once_flag guile_once;

std::string scheme_string(SCM value) {
    char* converted = scm_to_utf8_string(value);
    if (converted == nullptr) {
        throw std::runtime_error("Guile failed to convert a string");
    }
    std::string result(converted);
    std::free(converted);
    return result;
}

bool scheme_false(SCM value) {
    return scm_to_bool(scm_eq_p(value, SCM_BOOL_F)) != 0;
}

bool scheme_true(SCM value) {
    return !scheme_false(value);
}

bool scheme_boolean(SCM value) {
    return (scm_is_bool)(value) != 0;
}

SCM entity_id(std::uint32_t slot, std::uint32_t generation) {
    SCM result = scm_c_make_vector(2, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_uint32(slot));
    scm_c_vector_set_x(result, 1, scm_from_uint32(generation));
    return result;
}

template <typename Tag>
EntityId<Tag> entity_id_from_scheme(SCM value, const char* caller, int position) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 2) {
        scm_wrong_type_arg_msg(caller, position, value, "two-element entity ID vector");
    }
    const SCM slot = scm_c_vector_ref(value, 0);
    const SCM generation = scm_c_vector_ref(value, 1);
    if (scm_is_unsigned_integer(slot, 0, std::numeric_limits<std::uint32_t>::max()) == 0 ||
        scm_is_unsigned_integer(generation, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, position, value, "two-element entity ID vector");
    }
    const EntityId<Tag> result{.slot = scm_to_uint32(slot),
                               .generation = scm_to_uint32(generation)};
    if (!result.valid()) {
        scm_misc_error(caller, "entity ID is invalid", SCM_EOL);
    }
    return result;
}

void raise_host_error(const char* caller, const std::string& message) {
    scm_misc_error(caller, "host operation failed: ~A",
                   scm_list_1(scm_from_utf8_string(message.c_str())));
}

void finalize_host(SCM object) {
    delete static_cast<HostLease*>(scm_foreign_object_ref(object, 0));
}

HostLease& require_host(SCM object, const char* caller) {
    scm_assert_foreign_object_type(host_type, object);
    auto* host = static_cast<HostLease*>(scm_foreign_object_ref(object, 0));
    if (host == nullptr || host->runtime == nullptr) {
        scm_misc_error(caller, "editor host capability has expired", SCM_EOL);
    }
    return *host;
}

CommandResult invoke_script_command(const std::shared_ptr<GuileState>& state,
                                    std::size_t command_index, CommandContext& context,
                                    const CommandInvocation& invocation);
bool script_command_enabled(const std::shared_ptr<GuileState>& state, std::size_t command_index,
                            const CommandContext& context);

// The Guile ABI fixes four adjacent SCM arguments; Scheme-level names and
// validation preserve their semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM define_command(SCM host_object, SCM name_value, SCM execute_value, SCM enabled_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("define-command!", 2, name_value, "string");
    }
    if (!scheme_true(scm_procedure_p(execute_value))) {
        scm_wrong_type_arg_msg("define-command!", 3, execute_value, "procedure");
    }
    if (!scheme_false(enabled_value) && !scheme_true(scm_procedure_p(enabled_value))) {
        scm_wrong_type_arg_msg("define-command!", 4, enabled_value, "procedure or #f");
    }
    HostLease& host = require_host(host_object, "define-command!");
    const std::shared_ptr<GuileState> state = host.state;
    if (!state || !state->active) {
        scm_misc_error("define-command!", "Guile runtime has expired", SCM_EOL);
    }

    try {
        const std::string name = scheme_string(name_value);
        const std::size_t command_index = state->commands.size();
        (void)scm_gc_protect_object(execute_value);
        if (!scheme_false(enabled_value)) {
            (void)scm_gc_protect_object(enabled_value);
        }
        bool appended = false;
        try {
            state->commands.push_back({.execute = execute_value, .enabled = enabled_value});
            appended = true;
            const std::weak_ptr<GuileState> weak = state;
            CommandRegistry::Enabled enabled;
            if (!scheme_false(enabled_value)) {
                enabled = [weak, command_index](const CommandContext& context) {
                    const std::shared_ptr<GuileState> locked = weak.lock();
                    return locked && script_command_enabled(locked, command_index, context);
                };
            }
            const CommandId command = host.runtime->commands().define(
                name,
                [weak, command_index](CommandContext& context,
                                      const CommandInvocation& invocation) -> CommandResult {
                    const std::shared_ptr<GuileState> locked = weak.lock();
                    if (!locked || !locked->active) {
                        return std::unexpected(CommandError{"Guile command runtime has expired"});
                    }
                    return invoke_script_command(locked, command_index, context, invocation);
                },
                std::move(enabled));
            ++host.commands_installed;
            return scm_from_uint32(command.value);
        } catch (...) {
            if (appended) {
                state->commands.pop_back();
            }
            (void)scm_gc_unprotect_object(execute_value);
            if (!scheme_false(enabled_value)) {
                (void)scm_gc_unprotect_object(enabled_value);
            }
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("define-command!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-command!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// names and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM bind_key_if_command(SCM host_object, SCM keymap_value, SCM keys_value, SCM command_value) {
    if (!scm_is_string(keymap_value)) {
        scm_wrong_type_arg_msg("bind-key-if-command!", 2, keymap_value, "string");
    }
    if (!scm_is_string(keys_value)) {
        scm_wrong_type_arg_msg("bind-key-if-command!", 3, keys_value, "string");
    }
    if (!scm_is_string(command_value)) {
        scm_wrong_type_arg_msg("bind-key-if-command!", 4, command_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "bind-key-if-command!");
        const std::string keymap_name = scheme_string(keymap_value);
        const std::string keys = scheme_string(keys_value);
        const std::string command_name = scheme_string(command_value);
        const std::optional<KeymapId> keymap = host.runtime->keymaps().find(keymap_name);
        if (!keymap) {
            scm_misc_error("bind-key-if-command!", "unknown keymap: ~S", scm_list_1(keymap_value));
        }
        const std::optional<CommandId> command = host.runtime->commands().find(command_name);
        if (!command) {
            return SCM_BOOL_F;
        }
        host.runtime->keymaps().bind(*keymap, keys, *command);
        ++host.bindings_installed;
        return SCM_BOOL_T;
    } catch (const std::exception& exception) {
        scm_misc_error("bind-key-if-command!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("bind-key-if-command!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM buffer_id_by_name(SCM host_object, SCM name_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("buffer-id-by-name", 2, name_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "buffer-id-by-name");
        const std::optional<BufferId> buffer =
            host.runtime->buffers().find_by_name(scheme_string(name_value));
        return buffer ? entity_id(buffer->slot, buffer->generation) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-id-by-name", exception.what());
    } catch (...) {
        scm_misc_error("buffer-id-by-name", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM buffer_resource(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-resource");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-resource", 2);
        const std::optional<std::string>& resource =
            host.runtime->buffers().get(buffer).resource_uri();
        return resource ? scm_from_utf8_string(resource->c_str()) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-resource", exception.what());
    } catch (...) {
        scm_misc_error("buffer-resource", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM path_parent(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-parent", 2, path_value, "string");
    }
    (void)require_host(host_object, "path-parent");
    try {
        const std::string parent =
            std::filesystem::path(scheme_string(path_value)).parent_path().string();
        return scm_from_utf8_string(parent.c_str());
    } catch (const std::exception& exception) {
        raise_host_error("path-parent", exception.what());
    } catch (...) {
        scm_misc_error("path-parent", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM directory_path_p(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("directory-path?", 2, path_value, "string");
    }
    (void)require_host(host_object, "directory-path?");
    try {
        const std::string path = scheme_string(path_value);
        return scm_from_bool(!path.empty() &&
                             path.back() == std::filesystem::path::preferred_separator);
    } catch (const std::exception& exception) {
        raise_host_error("directory-path?", exception.what());
    } catch (...) {
        scm_misc_error("directory-path?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM path_as_directory(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-as-directory", 2, path_value, "string");
    }
    (void)require_host(host_object, "path-as-directory");
    try {
        std::string path =
            std::filesystem::path(scheme_string(path_value)).lexically_normal().string();
        if (path.empty() || path.back() != std::filesystem::path::preferred_separator) {
            path.push_back(std::filesystem::path::preferred_separator);
        }
        return scm_from_utf8_string(path.c_str());
    } catch (const std::exception& exception) {
        raise_host_error("path-as-directory", exception.what());
    } catch (...) {
        scm_misc_error("path-as-directory", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM display_buffer(SCM host_object, SCM window_value, SCM buffer_value) {
    HostLease& host = require_host(host_object, "display-buffer!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "display-buffer!", 2);
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "display-buffer!", 3);
    if (!host.services.display_buffer) {
        scm_misc_error("display-buffer!", "display-buffer capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> displayed =
            host.services.display_buffer(window, buffer);
        if (!displayed) {
            raise_host_error("display-buffer!", displayed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("display-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("display-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM move_caret_to_line(SCM host_object, SCM view_value, SCM line_value, SCM column_value) {
    HostLease& host = require_host(host_object, "move-caret-to-line!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "move-caret-to-line!", 2);
    if (scm_is_unsigned_integer(line_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("move-caret-to-line!", 3, line_value, "unsigned 32-bit integer");
    }
    if (scm_is_unsigned_integer(column_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("move-caret-to-line!", 4, column_value, "unsigned 32-bit integer");
    }
    if (!host.services.move_caret_to_line) {
        scm_misc_error("move-caret-to-line!", "caret capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> moved = host.services.move_caret_to_line(
            view, scm_to_uint32(line_value), scm_to_uint32(column_value));
        if (!moved) {
            raise_host_error("move-caret-to-line!", moved.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("move-caret-to-line!", exception.what());
    } catch (...) {
        scm_misc_error("move-caret-to-line!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM set_message(SCM host_object, SCM message_value) {
    if (!scm_is_string(message_value)) {
        scm_wrong_type_arg_msg("set-message!", 2, message_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "set-message!");
        if (!host.services.set_message) {
            scm_misc_error("set-message!", "message capability is unavailable", SCM_EOL);
        }
        host.services.set_message(scheme_string(message_value));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-message!", exception.what());
    } catch (...) {
        scm_misc_error("set-message!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM ensure_project_index(SCM host_object, SCM project_value) {
    HostLease& host = require_host(host_object, "ensure-project-index!");
    const ProjectId project =
        entity_id_from_scheme<ProjectTag>(project_value, "ensure-project-index!", 2);
    if (!host.services.ensure_project_index) {
        scm_misc_error("ensure-project-index!", "project-index capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> requested =
            host.services.ensure_project_index(project);
        if (!requested) {
            raise_host_error("ensure-project-index!", requested.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("ensure-project-index!", exception.what());
    } catch (...) {
        scm_misc_error("ensure-project-index!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM open_file(SCM host_object, SCM window_value, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("open-file!", 3, path_value, "string");
    }
    HostLease& host = require_host(host_object, "open-file!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "open-file!", 2);
    if (!host.services.open_file) {
        scm_misc_error("open-file!", "file-open capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> opened =
            host.services.open_file(window, scheme_string(path_value));
        if (!opened) {
            raise_host_error("open-file!", opened.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("open-file!", exception.what());
    } catch (...) {
        scm_misc_error("open-file!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM start_project_search(SCM host_object, SCM project_value, SCM window_value, SCM query_value) {
    if (!scm_is_string(query_value)) {
        scm_wrong_type_arg_msg("start-project-search!", 4, query_value, "string");
    }
    HostLease& host = require_host(host_object, "start-project-search!");
    const ProjectId project =
        entity_id_from_scheme<ProjectTag>(project_value, "start-project-search!", 2);
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "start-project-search!", 3);
    if (!host.services.start_project_search) {
        scm_misc_error("start-project-search!", "project-search capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<void, std::string> started =
            host.services.start_project_search(project, window, scheme_string(query_value));
        if (!started) {
            raise_host_error("start-project-search!", started.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("start-project-search!", exception.what());
    } catch (...) {
        scm_misc_error("start-project-search!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM set_buffer_resource(SCM host_object, SCM buffer_value, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("set-buffer-resource!", 3, path_value, "string");
    }
    HostLease& host = require_host(host_object, "set-buffer-resource!");
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-resource!", 2);
    if (!host.services.set_buffer_resource) {
        scm_misc_error("set-buffer-resource!", "buffer-resource capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<void, std::string> updated =
            host.services.set_buffer_resource(buffer, scheme_string(path_value));
        if (!updated) {
            raise_host_error("set-buffer-resource!", updated.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-buffer-resource!", exception.what());
    } catch (...) {
        scm_misc_error("set-buffer-resource!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM save_buffer(SCM host_object, SCM buffer_value) {
    HostLease& host = require_host(host_object, "save-buffer!");
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "save-buffer!", 2);
    if (!host.services.save_buffer) {
        scm_misc_error("save-buffer!", "buffer-save capability is unavailable", SCM_EOL);
    }
    try {
        host.services.save_buffer(buffer);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("save-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("save-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM open_buffers(SCM host_object) {
    HostLease& host = require_host(host_object, "open-buffer-ids");
    if (!host.services.open_buffers) {
        scm_misc_error("open-buffer-ids", "open-buffer snapshot capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::vector<BufferId> buffers = host.services.open_buffers();
        SCM result = scm_c_make_vector(buffers.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            scm_c_vector_set_x(result, index,
                               entity_id(buffers[index].slot, buffers[index].generation));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("open-buffer-ids", exception.what());
    } catch (...) {
        scm_misc_error("open-buffer-ids", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM kill_buffer(SCM host_object, SCM buffer_value, SCM force_value) {
    if (!scheme_boolean(force_value)) {
        scm_wrong_type_arg_msg("kill-buffer!", 3, force_value, "boolean");
    }
    HostLease& host = require_host(host_object, "kill-buffer!");
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "kill-buffer!", 2);
    if (!host.services.kill_buffer) {
        scm_misc_error("kill-buffer!", "buffer-kill capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> removed =
            host.services.kill_buffer(buffer, scheme_true(force_value));
        if (!removed) {
            return scm_from_utf8_string(removed.error().c_str());
        }
        return SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("kill-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("kill-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

void initialize_host_module(void*) {
    host_type = scm_make_foreign_object_type(scm_from_utf8_symbol("cind-editor-host"),
                                             scm_list_1(scm_from_utf8_symbol("implementation")),
                                             finalize_host);
    (void)scm_gc_protect_object(host_type);
    (void)scm_c_define_gsubr("define-command!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_command));
    (void)scm_c_define_gsubr("bind-key-if-command!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(bind_key_if_command));
    (void)scm_c_define_gsubr("buffer-id-by-name", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_id_by_name));
    (void)scm_c_define_gsubr("buffer-resource", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_resource));
    (void)scm_c_define_gsubr("path-parent", 2, 0, 0, reinterpret_cast<scm_t_subr>(path_parent));
    (void)scm_c_define_gsubr("directory-path?", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(directory_path_p));
    (void)scm_c_define_gsubr("path-as-directory", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(path_as_directory));
    (void)scm_c_define_gsubr("display-buffer!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(display_buffer));
    (void)scm_c_define_gsubr("move-caret-to-line!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(move_caret_to_line));
    (void)scm_c_define_gsubr("set-message!", 2, 0, 0, reinterpret_cast<scm_t_subr>(set_message));
    (void)scm_c_define_gsubr("ensure-project-index!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(ensure_project_index));
    (void)scm_c_define_gsubr("open-file!", 3, 0, 0, reinterpret_cast<scm_t_subr>(open_file));
    (void)scm_c_define_gsubr("start-project-search!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(start_project_search));
    (void)scm_c_define_gsubr("set-buffer-resource!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_resource));
    (void)scm_c_define_gsubr("save-buffer!", 2, 0, 0, reinterpret_cast<scm_t_subr>(save_buffer));
    (void)scm_c_define_gsubr("open-buffer-ids", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(open_buffers));
    (void)scm_c_define_gsubr("kill-buffer!", 3, 0, 0, reinterpret_cast<scm_t_subr>(kill_buffer));
    scm_c_export("define-command!", "bind-key-if-command!", "buffer-id-by-name", "buffer-resource",
                 "path-parent", "directory-path?", "path-as-directory", "display-buffer!",
                 "move-caret-to-line!", "set-message!", "ensure-project-index!", "open-file!",
                 "start-project-search!", "set-buffer-resource!", "save-buffer!", "open-buffer-ids",
                 "kill-buffer!", nullptr);
}

void initialize_guile() {
    scm_init_guile();
    (void)scm_c_define_module("cind host", initialize_host_module, nullptr);
}

struct GuileCall {
    enum class Operation : std::uint8_t {
        Load,
        InstallCommands,
        InstallKeymaps,
        InvokeCommand,
        CheckEnabled,
    };

    Operation operation = Operation::Load;
    std::string path;
    SCM host = SCM_UNDEFINED;
    SCM procedure = SCM_UNDEFINED;
    SCM result = SCM_UNDEFINED;
    std::size_t count = 0;
    const CommandContext* context = nullptr;
    const CommandInvocation* invocation = nullptr;
    std::optional<CommandResult> command_result;
    bool enabled = false;
    std::exception_ptr cpp_failure;
    std::string error;
};

SCM command_context_value(const CommandContext& context) {
    const std::optional<ProjectId> project = context.project_id();
    return scm_list_4(
        scm_cons(scm_from_utf8_symbol("window"),
                 entity_id(context.window_id().slot, context.window_id().generation)),
        scm_cons(scm_from_utf8_symbol("buffer"),
                 entity_id(context.buffer_id().slot, context.buffer_id().generation)),
        scm_cons(scm_from_utf8_symbol("view"),
                 entity_id(context.view_id().slot, context.view_id().generation)),
        scm_cons(scm_from_utf8_symbol("project"),
                 project ? entity_id(project->slot, project->generation) : SCM_BOOL_F));
}

SCM setting_value(const SettingValue& value) {
    if (const bool* boolean = std::get_if<bool>(&value)) {
        return scm_from_bool(*boolean);
    }
    if (const std::int64_t* integer = std::get_if<std::int64_t>(&value)) {
        return scm_from_int64(*integer);
    }
    if (const double* real = std::get_if<double>(&value)) {
        return scm_from_double(*real);
    }
    return scm_from_utf8_string(std::get<std::string>(value).c_str());
}

SCM command_invocation_value(const CommandInvocation& invocation) {
    SCM arguments = scm_c_make_vector(invocation.arguments.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < invocation.arguments.size(); ++index) {
        scm_c_vector_set_x(arguments, index, setting_value(invocation.arguments[index]));
    }
    SCM result = scm_c_make_vector(3, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("invocation"));
    scm_c_vector_set_x(result, 1, arguments);
    scm_c_vector_set_x(
        result, 2, invocation.repeat_count ? scm_from_int64(*invocation.repeat_count) : SCM_BOOL_F);
    return result;
}

bool setting_value_p(SCM value) {
    if (scheme_boolean(value) || scm_is_string(value)) {
        return true;
    }
    if (scm_is_integer(value)) {
        return scm_is_signed_integer(value, std::numeric_limits<std::int64_t>::min(),
                                     std::numeric_limits<std::int64_t>::max()) != 0;
    }
    return scm_is_real(value) != 0;
}

SettingValue setting_from_scheme(SCM value) {
    if (scheme_boolean(value)) {
        return scheme_true(value);
    }
    if (scm_is_integer(value)) {
        return scm_to_int64(value);
    }
    if (scm_is_real(value)) {
        return scm_to_double(value);
    }
    return scheme_string(value);
}

std::vector<SettingValue> arguments_from_scheme(SCM values, const char* caller) {
    if (!scm_is_vector(values)) {
        scm_wrong_type_arg_msg(caller, 0, values, "vector of command arguments");
    }
    const std::size_t size = scm_c_vector_length(values);
    for (std::size_t index = 0; index < size; ++index) {
        if (!setting_value_p(scm_c_vector_ref(values, index))) {
            scm_misc_error(caller, "command argument has an unsupported type", SCM_EOL);
        }
    }
    std::vector<SettingValue> result;
    result.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(setting_from_scheme(scm_c_vector_ref(values, index)));
    }
    return result;
}

bool symbol_is(SCM value, const char* expected) {
    return scheme_true(scm_symbol_p(value)) &&
           scheme_true(scm_eq_p(value, scm_from_utf8_symbol(expected)));
}

CommandResult command_result_from_scheme(SCM value, const CommandContext& context) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) == 0) {
        return std::unexpected(CommandError{"Guile command returned an invalid result"});
    }
    const std::size_t size = scm_c_vector_length(value);
    const SCM tag = scm_c_vector_ref(value, 0);
    if (symbol_is(tag, "completed")) {
        if (size > 2 || (size == 2 && !setting_value_p(scm_c_vector_ref(value, 1)))) {
            return std::unexpected(CommandError{"Guile completed result is malformed"});
        }
        CommandCompleted completed_result;
        if (size == 2) {
            completed_result.value = setting_from_scheme(scm_c_vector_ref(value, 1));
        }
        return completed_result;
    }
    if (symbol_is(tag, "error")) {
        if (size != 2 || !scm_is_string(scm_c_vector_ref(value, 1))) {
            return std::unexpected(CommandError{"Guile error result is malformed"});
        }
        return std::unexpected(CommandError{scheme_string(scm_c_vector_ref(value, 1))});
    }
    if (symbol_is(tag, "dispatch")) {
        if (size != 3 || !scm_is_string(scm_c_vector_ref(value, 1)) ||
            !scm_is_vector(scm_c_vector_ref(value, 2))) {
            return std::unexpected(CommandError{"Guile dispatch result is malformed"});
        }
        std::vector<SettingValue> arguments =
            arguments_from_scheme(scm_c_vector_ref(value, 2), "command-dispatch");
        const std::string name = scheme_string(scm_c_vector_ref(value, 1));
        const std::optional<CommandId> command = context.runtime().commands().find(name);
        if (!command) {
            return std::unexpected(CommandError{std::format("unknown command '{}'", name)});
        }
        return CommandDispatch{
            .command = *command,
            .invocation = {.arguments = std::move(arguments), .repeat_count = std::nullopt}};
    }
    if (symbol_is(tag, "interaction")) {
        if (size != 9 || !scheme_true(scm_symbol_p(scm_c_vector_ref(value, 1))) ||
            !scm_is_string(scm_c_vector_ref(value, 2)) ||
            !scm_is_string(scm_c_vector_ref(value, 3)) ||
            !scm_is_string(scm_c_vector_ref(value, 4)) ||
            !scm_is_string(scm_c_vector_ref(value, 5)) ||
            !scheme_boolean(scm_c_vector_ref(value, 6)) ||
            !scm_is_string(scm_c_vector_ref(value, 7)) ||
            !scm_is_vector(scm_c_vector_ref(value, 8))) {
            return std::unexpected(CommandError{"Guile interaction result is malformed"});
        }
        const SCM kind_value = scm_c_vector_ref(value, 1);
        InteractionKind kind;
        if (symbol_is(kind_value, "text")) {
            kind = InteractionKind::Text;
        } else if (symbol_is(kind_value, "picker")) {
            kind = InteractionKind::Picker;
        } else {
            return std::unexpected(CommandError{"Guile interaction kind is unknown"});
        }
        std::vector<SettingValue> arguments =
            arguments_from_scheme(scm_c_vector_ref(value, 8), "command-interaction");
        const std::string accept_name = scheme_string(scm_c_vector_ref(value, 7));
        const std::optional<CommandId> accept = context.runtime().commands().find(accept_name);
        if (!accept) {
            return std::unexpected(
                CommandError{std::format("unknown accept command '{}'", accept_name)});
        }
        return InteractionRequest{.kind = kind,
                                  .prompt = scheme_string(scm_c_vector_ref(value, 2)),
                                  .initial_input = scheme_string(scm_c_vector_ref(value, 3)),
                                  .history = scheme_string(scm_c_vector_ref(value, 4)),
                                  .provider = scheme_string(scm_c_vector_ref(value, 5)),
                                  .allow_custom_input = scheme_true(scm_c_vector_ref(value, 6)),
                                  .accept_command = *accept,
                                  .arguments = std::move(arguments)};
    }
    return std::unexpected(CommandError{"Guile command returned an unknown result kind"});
}

SCM call_body(void* data) {
    auto& call = *static_cast<GuileCall*>(data);
    try {
        switch (call.operation) {
        case GuileCall::Operation::Load:
            call.result = scm_c_primitive_load(call.path.c_str());
            break;
        case GuileCall::Operation::InstallCommands:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-core-commands!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallKeymaps:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-default-keymaps!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InvokeCommand:
            call.result = scm_call_2(call.procedure, command_context_value(*call.context),
                                     command_invocation_value(*call.invocation));
            call.command_result = command_result_from_scheme(call.result, *call.context);
            break;
        case GuileCall::Operation::CheckEnabled:
            call.result = scm_call_1(call.procedure, command_context_value(*call.context));
            call.enabled = scheme_true(call.result);
            break;
        }
    } catch (...) {
        call.cpp_failure = std::current_exception();
    }
    return call.result;
}

SCM call_handler(void* data, SCM tag, SCM arguments) {
    auto& call = *static_cast<GuileCall*>(data);
    try {
        const SCM message = scm_simple_format(SCM_BOOL_F, scm_from_utf8_string("~S: ~S"),
                                              scm_list_2(tag, arguments));
        call.error = scheme_string(message);
    } catch (...) {
        call.cpp_failure = std::current_exception();
    }
    return SCM_UNSPECIFIED;
}

std::expected<SCM, std::string> run_guile_call(GuileCall& call) {
    (void)scm_c_catch(SCM_BOOL_T, call_body, &call, call_handler, &call, nullptr, nullptr);
    if (call.cpp_failure) {
        try {
            std::rethrow_exception(call.cpp_failure);
        } catch (const std::exception& exception) {
            return std::unexpected(std::format("C++ Guile bridge failure: {}", exception.what()));
        } catch (...) {
            return std::unexpected("unknown C++ Guile bridge failure");
        }
    }
    if (!call.error.empty()) {
        return std::unexpected(std::move(call.error));
    }
    return call.result;
}

std::filesystem::path bundled_module_path(std::string_view name) {
    return std::filesystem::path(CIND_BUNDLED_SCHEME_DIR) / "cind" / std::format("{}.scm", name);
}

CommandResult invoke_script_command(const std::shared_ptr<GuileState>& state,
                                    std::size_t command_index, CommandContext& context,
                                    const CommandInvocation& invocation) {
    if (std::this_thread::get_id() != state->owner) {
        return std::unexpected(CommandError{"Guile command invoked outside its editor thread"});
    }
    if (!state->active || command_index >= state->commands.size()) {
        return std::unexpected(CommandError{"Guile command runtime has expired"});
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeCommand;
    call.procedure = state->commands[command_index].execute;
    call.context = &context;
    call.invocation = &invocation;
    std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        return std::unexpected(
            CommandError{std::format("Guile command failed: {}", result.error())});
    }
    if (!call.command_result) {
        state->last_error = "Guile command produced no result";
        return std::unexpected(CommandError{*state->last_error});
    }
    state->last_error.reset();
    return std::move(*call.command_result);
}

bool script_command_enabled(const std::shared_ptr<GuileState>& state, std::size_t command_index,
                            const CommandContext& context) {
    if (std::this_thread::get_id() != state->owner || !state->active ||
        command_index >= state->commands.size()) {
        state->last_error = "Guile command predicate used outside its runtime";
        return false;
    }
    GuileCall call;
    call.operation = GuileCall::Operation::CheckEnabled;
    call.procedure = state->commands[command_index].enabled;
    call.context = &context;
    std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        return false;
    }
    state->last_error.reset();
    return call.enabled;
}

} // namespace

class GuileRuntime::Impl {
public:
    explicit Impl(EditorRuntime& runtime, GuileHostServices services)
        : state_(std::make_shared<GuileState>()) {
        state_->owner = std::this_thread::get_id();
        std::call_once(guile_once, initialize_guile);
        for (std::string_view module : {"command", "core"}) {
            GuileCall load;
            load.operation = GuileCall::Operation::Load;
            load.path = bundled_module_path(module).string();
            if (std::expected<SCM, std::string> loaded = run_guile_call(load); !loaded) {
                state_->last_error = loaded.error();
                throw std::runtime_error(std::format("failed to load bundled Guile module '{}': {}",
                                                     module, *state_->last_error));
            }
        }
        version_ = scheme_string(scm_version());
        lease_ =
            new HostLease{.runtime = &runtime, .state = state_, .services = std::move(services)};
        host_ = scm_make_foreign_object_1(host_type, lease_);
        (void)scm_gc_protect_object(host_);
    }

    ~Impl() {
        state_->active = false;
        for (const ScriptCommand& command : state_->commands) {
            (void)scm_gc_unprotect_object(command.execute);
            if (!scheme_false(command.enabled)) {
                (void)scm_gc_unprotect_object(command.enabled);
            }
        }
        state_->commands.clear();
        lease_->runtime = nullptr;
        lease_->state.reset();
        scm_foreign_object_set_x(host_, 0, nullptr);
        (void)scm_gc_unprotect_object(host_);
        delete std::exchange(lease_, nullptr);
    }

    std::expected<std::size_t, std::string> install_core_commands() {
        require_owner_thread();
        lease_->commands_installed = 0;
        GuileCall call;
        call.operation = GuileCall::Operation::InstallCommands;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != lease_->commands_installed || call.count != state_->commands.size()) {
            state_->last_error = "Guile command policy returned an inconsistent command count";
            return std::unexpected(*state_->last_error);
        }
        ++state_->command_revision;
        state_->last_error.reset();
        return call.count;
    }

    std::expected<std::size_t, std::string> install_default_keymaps() {
        require_owner_thread();
        lease_->bindings_installed = 0;
        GuileCall call;
        call.operation = GuileCall::Operation::InstallKeymaps;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        const std::size_t installed = call.count;
        if (installed != lease_->bindings_installed) {
            state_->last_error = "Guile keymap policy returned an inconsistent binding count";
            return std::unexpected(*state_->last_error);
        }
        ++binding_revision_;
        state_->last_error.reset();
        return installed;
    }

    GuileRuntimeSnapshot snapshot() const {
        return {.engine = "guile",
                .version = version_,
                .modules = {"cind command", "cind core"},
                .command_revision = state_->command_revision,
                .scripted_commands = state_->commands.size(),
                .binding_revision = binding_revision_,
                .last_error = state_->last_error};
    }

private:
    void require_owner_thread() const {
        if (std::this_thread::get_id() != state_->owner) {
            throw std::logic_error("Guile runtime must run on its editor thread");
        }
    }

    std::shared_ptr<GuileState> state_;
    HostLease* lease_ = nullptr;
    SCM host_ = SCM_UNDEFINED;
    std::string version_;
    std::uint64_t binding_revision_ = 0;
};

GuileRuntime::GuileRuntime(EditorRuntime& runtime, GuileHostServices services)
    : impl_(std::make_unique<Impl>(runtime, std::move(services))) {}

GuileRuntime::~GuileRuntime() = default;

std::expected<std::size_t, std::string> GuileRuntime::install_core_commands() {
    return impl_->install_core_commands();
}

std::expected<std::size_t, std::string> GuileRuntime::install_default_keymaps() {
    return impl_->install_default_keymaps();
}

GuileRuntimeSnapshot GuileRuntime::snapshot() const {
    return impl_->snapshot();
}

} // namespace cind
