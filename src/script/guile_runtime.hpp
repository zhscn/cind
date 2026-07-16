#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cind {

class EditorRuntime;

struct GuileRuntimeSnapshot {
    std::string engine;
    std::string version;
    std::vector<std::string> modules;
    std::uint64_t binding_revision = 0;
    std::optional<std::string> last_error;
};

// Owns the editor-thread Guile policy environment. C++ registries and
// generational editor objects remain authoritative; Scheme receives only
// explicit host capabilities and never a process-global current editor.
class GuileRuntime {
public:
    explicit GuileRuntime(EditorRuntime& runtime);
    ~GuileRuntime();

    GuileRuntime(const GuileRuntime&) = delete;
    GuileRuntime& operator=(const GuileRuntime&) = delete;

    std::expected<std::size_t, std::string> install_default_keymaps();
    GuileRuntimeSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind
