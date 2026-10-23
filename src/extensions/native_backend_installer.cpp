#include "extensions/native_backend_installer.hpp"

#include "extensions/extension_store.hpp"

namespace edgedesk::extensions {

NativeBackendInstaller::NativeBackendInstaller(ExtensionStore& store, std::string apps_root)
    : store_(store), apps_root_(std::move(apps_root)) {}

// APPX-02 implements the install/upgrade/uninstall file lifecycle. These
// stubs keep the shared contract linkable for the other workstreams.
Error NativeBackendInstaller::validate(const std::string& extracted_root, const Manifest& manifest,
                                       BackendRecord& out) {
    (void)extracted_root;
    (void)manifest;
    (void)out;
    return Error::make(ErrorCode::unsupported, "native backend install is not implemented");
}

Error NativeBackendInstaller::install(const std::string& revision_root, const Manifest& manifest,
                                      const BackendRecord* previous, BackendRecord& out,
                                      std::string& warning) {
    (void)revision_root;
    (void)manifest;
    (void)previous;
    (void)out;
    (void)warning;
    return Error::make(ErrorCode::unsupported, "native backend install is not implemented");
}

Error NativeBackendInstaller::remove(std::string_view app_id, bool purge_data) {
    (void)app_id;
    (void)purge_data;
    return Error::make(ErrorCode::unsupported, "native backend install is not implemented");
}

Error NativeBackendInstaller::purge_data(std::string_view app_id) {
    (void)app_id;
    return Error::make(ErrorCode::unsupported, "native backend install is not implemented");
}

Error NativeBackendInstaller::verify(std::string_view app_id, BackendRecord& out) const {
    (void)app_id;
    (void)out;
    return Error::make(ErrorCode::unsupported, "native backend install is not implemented");
}

} // namespace edgedesk::extensions
