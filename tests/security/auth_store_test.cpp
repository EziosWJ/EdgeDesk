#include "security/auth_store.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <sys/stat.h>

using namespace edgedesk;

namespace {

AuthRecord dummy_record() {
    AuthRecord record;
    record.salt_hex = std::string(32, 'a');
    record.hash_hex = std::string(64, 'b');
    record.credential_version = 1;
    record.created_at = 123;
    return record;
}

mode_t mode_of(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return info.st_mode & 0777;
}

} // namespace

int main() {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("edgedesk-auth-store-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    const auto data = (tmp / "data").string();

    {
        std::string error;
        auto store = AuthStore::open(data, true, error);
        CHECK(store != nullptr);
        if (!store) return test::finish("auth_store_test");
        CHECK(store->state() == AuthState::uninitialized);
        CHECK(mode_of(data) == 0700);

        CHECK(store->save_record(dummy_record(), error));
        CHECK(store->state() == AuthState::initialized);
        CHECK(mode_of(data + "/auth.json") == 0600);
        AuthRecord loaded;
        CHECK(store->read_record(loaded, error));
        CHECK_STR_EQ(loaded.salt_hex, dummy_record().salt_hex);
        CHECK_EQ(loaded.credential_version, std::uint64_t{1});

        // The same directory cannot be opened twice while the lock is held.
        auto second = AuthStore::open(data, true, error);
        CHECK(second == nullptr);
    }

    {
        std::string error;
        auto store = AuthStore::open(data, false, error);
        CHECK(store != nullptr);
        if (!store) return test::finish("auth_store_test");
        CHECK(store->state() == AuthState::initialized);
        CHECK(store->reset_for_recovery(error));
        CHECK(store->state() == AuthState::uninitialized);
    }

    // Corrupt auth.json must not be interpreted as a fresh install.
    {
        std::ofstream bad(data + "/auth.json");
        bad << "{not-json";
    }
    {
        std::string error;
        auto store = AuthStore::open(data, false, error);
        CHECK(store != nullptr);
        if (!store) return test::finish("auth_store_test");
        CHECK(store->state() == AuthState::corrupt);
        CHECK(!store->last_error().empty());
        CHECK(store->reset_for_recovery(error));
        CHECK(store->state() == AuthState::uninitialized);
    }

    std::filesystem::remove_all(tmp);
    return test::finish("auth_store_test");
}
