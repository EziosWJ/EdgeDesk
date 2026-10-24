#include "test_util.hpp"

#include "extensions/native_backend.hpp"

using namespace edgedesk::extensions;

namespace {

void test_paths() {
    const std::string root = "/var/lib/edgedesk/apps";
    CHECK_STR_EQ(backend_program_dir(root, "io.example.app"), root + "/native/io.example.app");
    CHECK_STR_EQ(backend_executable_path(root, "io.example.app"),
                 root + "/native/io.example.app/backend");
    CHECK_STR_EQ(backend_data_dir(root, "io.example.app"), root + "/data/io.example.app");
    CHECK_STR_EQ(backend_socket_path(root, "io.example.app"),
                 root + "/run/io.example.app/backend.sock");
}

void test_service_id() {
    const std::string id = backend_service_id("io.example.app");
    CHECK(id.rfind("app-", 0) == 0);
    CHECK_EQ(id.size(), std::size_t{36});
    CHECK_STR_EQ(id, backend_service_id("io.example.app"));
    CHECK(id != backend_service_id("io.example.other"));
}

} // namespace

int main() {
    test_paths();
    test_service_id();
    return edgedesk::test::finish("native_backend_test");
}
