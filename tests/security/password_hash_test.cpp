#include "security/password_hash.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace edgedesk;

int main() {
    std::string error;
    CHECK(validate_password("correct horse battery", error));
    CHECK(!validate_password("short", error));
    CHECK(!validate_password(std::string(1025, 'a'), error));
    CHECK(!validate_password(std::string("hello\nworld!!!"), error));

    std::string salt;
    std::string hash;
    Error hash_error;
    CHECK(hash_password("correct horse battery", 1024, 1, 1, salt, hash, hash_error));
    CHECK_EQ(salt.size(), std::size_t{32});
    CHECK_EQ(hash.size(), std::size_t{64});
    AuthRecord record;
    record.memory_kib = 1024;
    record.iterations = 1;
    record.parallelism = 1;
    record.salt_hex = salt;
    record.hash_hex = hash;
    bool matches = false;
    CHECK(verify_password("correct horse battery", record, matches, hash_error));
    CHECK(matches);
    CHECK(verify_password("wrong password!!", record, matches, hash_error));
    CHECK(!matches);
    CHECK(password_needs_rehash(record)); // low test parameters differ from baseline

    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    Error worker_error;
    std::string worker_salt;
    std::string worker_hash;
    PasswordHashService service(1, 2);
    CHECK(service.hash_async("correct horse battery",
                             [&](Error result, std::string next_salt, std::string next_hash) {
                                 std::lock_guard<std::mutex> lock(mutex);
                                 worker_error = result;
                                 worker_salt = std::move(next_salt);
                                 worker_hash = std::move(next_hash);
                                 done = true;
                                 ready.notify_one();
                             }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait_for(lock, std::chrono::seconds(10), [&] { return done; });
    }
    CHECK(done);
    CHECK(worker_error.ok());
    CHECK_EQ(worker_salt.size(), std::size_t{32});
    CHECK_EQ(worker_hash.size(), std::size_t{64});

    done = false;
    bool worker_matches = false;
    CHECK(service.verify_async("correct horse battery", record,
                               [&](Error result, bool next_matches) {
                                   std::lock_guard<std::mutex> lock(mutex);
                                   worker_error = result;
                                   worker_matches = next_matches;
                                   done = true;
                                   ready.notify_one();
                               }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait_for(lock, std::chrono::seconds(10), [&] { return done; });
    }
    CHECK(done);
    CHECK(worker_error.ok());
    CHECK(worker_matches);

    return test::finish("password_hash_test");
}
