#pragma once

#include "script/async_host.hpp"

#include <libguile.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cind {

struct GuileAsyncBridgeState;

ScriptAsyncRequest script_async_request_from_scheme(SCM value, const char* caller, int position);
ScriptLspProviderSpec script_lsp_provider_from_scheme(SCM value, const char* caller, int position);
SCM script_async_result_to_scheme(ScriptAsyncResult result);

class GuileAsyncBridge {
public:
    using Start = std::function<std::expected<std::uint64_t, std::string>(ScriptAsyncRequest,
                                                                          ScriptAsyncCallbacks)>;
    using Cancel = std::function<bool(std::uint64_t)>;
    using Inspect = std::function<std::vector<ScriptAsyncTaskSummary>()>;
    using ReportError = std::function<void(std::string)>;

    GuileAsyncBridge(Start start, Cancel cancel, Inspect inspect, ReportError report_error);
    ~GuileAsyncBridge();

    GuileAsyncBridge(const GuileAsyncBridge&) = delete;
    GuileAsyncBridge& operator=(const GuileAsyncBridge&) = delete;

    SCM start(SCM request, SCM completed, SCM failed, SCM cancelled);
    SCM cancel(SCM task);
    SCM summaries() const;

    std::expected<std::uint64_t, std::string> start_native_task(ScriptAsyncRequest request,
                                                                ScriptAsyncCallbacks callbacks);
    bool cancel_native_task(std::uint64_t task);

    std::size_t outstanding() const;
    std::vector<std::uint64_t> checkpoint() const;
    void rollback_to(std::span<const std::uint64_t> checkpoint) noexcept;
    void shutdown() noexcept;

private:
    void release(bool cancel) noexcept;

    std::shared_ptr<GuileAsyncBridgeState> state_;
};

using GuileAsyncBridgeResolver = GuileAsyncBridge& (*)(SCM host, const char* caller);

// Adds the normalized async task primitives to the current `(cind host)`
// module. The resolver retains host validation and capability ownership in
// the main Guile runtime.
void initialize_guile_async_host_bindings(GuileAsyncBridgeResolver resolver);

} // namespace cind
