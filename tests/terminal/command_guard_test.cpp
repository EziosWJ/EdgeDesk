#include "test_util.hpp"

#include "terminal/command_guard.hpp"

using namespace edgedesk;

namespace {

bool blocked(std::string_view line) { return CommandGuard::inspect(line).has_value(); }

void test_dangerous_lines() {
    CHECK(blocked("rm -rf /"));
    CHECK(blocked("sudo rm -rf / "));
    CHECK(blocked("rm -rf /*"));
    CHECK(blocked("rm -fr /etc"));
    CHECK(blocked("rm -rf ~/"));
    CHECK(blocked("rm -rf $HOME"));
    CHECK(blocked("dd if=/dev/zero of=/dev/sda"));
    CHECK(blocked("dd of=/dev/mmcblk0 bs=1M"));
    CHECK(blocked("mkfs.ext4 /dev/sda1"));
    CHECK(blocked("wipefs -a /dev/sda"));
    CHECK(blocked(":(){ :|:& };:"));
    CHECK(blocked("shutdown -h now"));
    CHECK(blocked("sudo reboot"));
    CHECK(blocked("kill -9 -1"));
    CHECK(blocked("killall5"));
    CHECK(blocked("chmod -R 777 /"));
    CHECK(blocked("fdisk /dev/sda"));
    CHECK(blocked("echo hacked > /etc/passwd"));
    CHECK(blocked("echo x > /etc/fstab"));
    CHECK(blocked("echo boom > /dev/mem"));
}

void test_safe_lines() {
    CHECK(!blocked(""));
    CHECK(!blocked("ls -la"));
    CHECK(!blocked("rm -rf /tmp/edgedesk-test"));
    CHECK(!blocked("rm -rf ./build"));
    CHECK(!blocked("echo shutdown"));
    CHECK(!blocked("cat /etc/passwd"));
    CHECK(!blocked("rebootnow"));
    CHECK(!blocked("chmod 755 /etc/hosts"));
    CHECK(!blocked("chown -R user:user /data"));
    CHECK(!blocked("df -h"));
    CHECK(!blocked("docker ps"));
    CHECK(!blocked("mkdir -p /var/tmp/x"));
    CHECK(!blocked("kill -9 12345"));
}

void test_line_tracking_reset() {
    CHECK(!CommandGuard::breaks_line_tracking('\r'));
    CHECK(!CommandGuard::breaks_line_tracking('\n'));
    CHECK(!CommandGuard::breaks_line_tracking(0x7f));
    CHECK(!CommandGuard::breaks_line_tracking(0x08));
    CHECK(CommandGuard::breaks_line_tracking(0x01)); // Ctrl+A
    CHECK(CommandGuard::breaks_line_tracking(0x1b)); // ESC
}

} // namespace

int main() {
    test_dangerous_lines();
    test_safe_lines();
    test_line_tracking_reset();
    return edgedesk::test::finish("command_guard_test");
}
