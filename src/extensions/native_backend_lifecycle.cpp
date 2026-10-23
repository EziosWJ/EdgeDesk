#include "extensions/native_backend_lifecycle.hpp"

#include "extensions/extension_store.hpp"
#include "extensions/native_backend_installer.hpp"
#include "extensions/package_manager.hpp"

namespace edgedesk::extensions {

NativeBackendLifecycle::NativeBackendLifecycle(ExtensionStore& store, PackageManager& packages,
                                               NativeBackendInstaller& installer,
                                               std::string apps_root)
    : store_(store), packages_(packages), installer_(installer), apps_root_(std::move(apps_root)) {}

// APPX-03 implements these; the stubs keep the shared contract linkable.
Error NativeBackendLifecycle::describe(std::string_view app_id, nlohmann::json& out) const {
    (void)app_id;
    (void)out;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

Error NativeBackendLifecycle::service_params(std::string_view app_id, nlohmann::json& params) const {
    (void)app_id;
    (void)params;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

Error NativeBackendLifecycle::prepare_runtime(std::string_view app_id, std::string& socket_path) {
    (void)app_id;
    (void)socket_path;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

Error NativeBackendLifecycle::clean_runtime(std::string_view app_id) {
    (void)app_id;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

Error NativeBackendLifecycle::set_identity(std::string_view app_id, const std::string& run_user,
                                           const std::string& run_group, bool allow_root) {
    (void)app_id;
    (void)run_user;
    (void)run_group;
    (void)allow_root;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

Error NativeBackendLifecycle::set_autostart(std::string_view app_id, bool enabled) {
    (void)app_id;
    (void)enabled;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

Error NativeBackendLifecycle::set_state(std::string_view app_id, std::string_view state,
                                       const std::string& error) {
    (void)app_id;
    (void)state;
    (void)error;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

Error NativeBackendLifecycle::service_id(std::string_view app_id, std::string& out, bool& found) const {
    (void)app_id;
    (void)out;
    (void)found;
    return Error::make(ErrorCode::unsupported, "native backend lifecycle is not implemented");
}

} // namespace edgedesk::extensions
