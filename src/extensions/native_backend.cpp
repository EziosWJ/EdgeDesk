#include "extensions/native_backend.hpp"

#include "core/util.hpp"

namespace edgedesk::extensions {

std::string backend_program_dir(std::string_view apps_root, std::string_view app_id) {
    return std::string(apps_root) + "/native/" + std::string(app_id);
}

std::string backend_executable_path(std::string_view apps_root, std::string_view app_id) {
    return backend_program_dir(apps_root, app_id) + "/" + kBackendExecutableName;
}

std::string backend_data_dir(std::string_view apps_root, std::string_view app_id) {
    return std::string(apps_root) + "/data/" + std::string(app_id);
}

std::string backend_run_dir(std::string_view apps_root, std::string_view app_id) {
    return std::string(apps_root) + "/run/" + std::string(app_id);
}

std::string backend_socket_path(std::string_view apps_root, std::string_view app_id) {
    return backend_run_dir(apps_root, app_id) + "/" + kBackendSocketName;
}

std::string backend_service_id(std::string_view app_id) {
    return "app-" + sha256_hex(app_id).substr(0, 32);
}

} // namespace edgedesk::extensions
