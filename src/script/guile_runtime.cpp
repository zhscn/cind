#include "script/guile_runtime.hpp"

#include "editor/runtime.hpp"

#include <libguile.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace cind {

namespace {

struct HostLease {
    EditorRuntime* runtime = nullptr;
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

void finalize_host(SCM object) {
    delete static_cast<HostLease*>(scm_foreign_object_ref(object, 0));
}

HostLease& require_host(SCM object) {
    scm_assert_foreign_object_type(host_type, object);
    auto* host = static_cast<HostLease*>(scm_foreign_object_ref(object, 0));
    if (host == nullptr || host->runtime == nullptr) {
        scm_misc_error("bind-key-if-command!", "editor host capability has expired", SCM_EOL);
    }
    return *host;
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
        HostLease& host = require_host(host_object);
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

void initialize_host_module(void*) {
    host_type = scm_make_foreign_object_type(scm_from_utf8_symbol("cind-editor-host"),
                                             scm_list_1(scm_from_utf8_symbol("implementation")),
                                             finalize_host);
    (void)scm_gc_protect_object(host_type);
    (void)scm_c_define_gsubr("bind-key-if-command!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(bind_key_if_command));
    scm_c_export("bind-key-if-command!", nullptr);
}

void initialize_guile() {
    scm_init_guile();
    (void)scm_c_define_module("cind host", initialize_host_module, nullptr);
}

struct GuileCall {
    enum class Operation : std::uint8_t {
        Load,
        InstallKeymaps,
    };

    Operation operation = Operation::Load;
    std::string path;
    SCM host = SCM_UNDEFINED;
    SCM result = SCM_UNDEFINED;
    std::size_t count = 0;
    std::string error;
};

SCM call_body(void* data) {
    auto& call = *static_cast<GuileCall*>(data);
    if (call.operation == GuileCall::Operation::Load) {
        call.result = scm_c_primitive_load(call.path.c_str());
    } else {
        const SCM procedure = scm_c_public_ref("cind core", "install-default-keymaps!");
        call.result = scm_call_1(procedure, call.host);
        call.count = scm_to_size_t(call.result);
    }
    return call.result;
}

SCM call_handler(void* data, SCM tag, SCM arguments) {
    auto& call = *static_cast<GuileCall*>(data);
    const SCM message =
        scm_simple_format(SCM_BOOL_F, scm_from_utf8_string("~S: ~S"), scm_list_2(tag, arguments));
    call.error = scheme_string(message);
    return SCM_UNSPECIFIED;
}

std::expected<SCM, std::string> run_guile_call(GuileCall& call) {
    (void)scm_c_catch(SCM_BOOL_T, call_body, &call, call_handler, &call, nullptr, nullptr);
    if (!call.error.empty()) {
        return std::unexpected(std::move(call.error));
    }
    return call.result;
}

std::filesystem::path core_module_path() {
    return std::filesystem::path(CIND_BUNDLED_SCHEME_DIR) / "cind" / "core.scm";
}

} // namespace

class GuileRuntime::Impl {
public:
    explicit Impl(EditorRuntime& runtime) : owner_(std::this_thread::get_id()) {
        std::call_once(guile_once, initialize_guile);
        GuileCall load{.operation = GuileCall::Operation::Load,
                       .path = core_module_path().string(),
                       .host = SCM_UNDEFINED,
                       .result = SCM_UNDEFINED,
                       .count = 0,
                       .error = {}};
        if (std::expected<SCM, std::string> loaded = run_guile_call(load); !loaded) {
            last_error_ = loaded.error();
            throw std::runtime_error(
                std::format("failed to load bundled Guile policy: {}", *last_error_));
        }
        version_ = scheme_string(scm_version());
        lease_ = new HostLease{.runtime = &runtime};
        host_ = scm_make_foreign_object_1(host_type, lease_);
        (void)scm_gc_protect_object(host_);
    }

    ~Impl() {
        lease_->runtime = nullptr;
        scm_foreign_object_set_x(host_, 0, nullptr);
        (void)scm_gc_unprotect_object(host_);
        delete std::exchange(lease_, nullptr);
    }

    std::expected<std::size_t, std::string> install_default_keymaps() {
        require_owner_thread();
        lease_->bindings_installed = 0;
        GuileCall call{.operation = GuileCall::Operation::InstallKeymaps,
                       .path = {},
                       .host = host_,
                       .result = SCM_UNDEFINED,
                       .count = 0,
                       .error = {}};
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            last_error_ = result.error();
            return std::unexpected(*last_error_);
        }
        const std::size_t installed = call.count;
        if (installed != lease_->bindings_installed) {
            last_error_ = "Guile keymap policy returned an inconsistent binding count";
            return std::unexpected(*last_error_);
        }
        ++binding_revision_;
        last_error_.reset();
        return installed;
    }

    GuileRuntimeSnapshot snapshot() const {
        return {.engine = "guile",
                .version = version_,
                .modules = {"cind core"},
                .binding_revision = binding_revision_,
                .last_error = last_error_};
    }

private:
    void require_owner_thread() const {
        if (std::this_thread::get_id() != owner_) {
            throw std::logic_error("Guile runtime must run on its editor thread");
        }
    }

    std::thread::id owner_;
    HostLease* lease_ = nullptr;
    SCM host_ = SCM_UNDEFINED;
    std::string version_;
    std::uint64_t binding_revision_ = 0;
    std::optional<std::string> last_error_;
};

GuileRuntime::GuileRuntime(EditorRuntime& runtime) : impl_(std::make_unique<Impl>(runtime)) {}

GuileRuntime::~GuileRuntime() = default;

std::expected<std::size_t, std::string> GuileRuntime::install_default_keymaps() {
    return impl_->install_default_keymaps();
}

GuileRuntimeSnapshot GuileRuntime::snapshot() const {
    return impl_->snapshot();
}

} // namespace cind
