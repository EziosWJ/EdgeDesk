#include "test_util.hpp"

#include "extensions/backend_proxy.hpp"

// APPX-04 fills this in with a real Unix-domain-socket HTTP server, timeout,
// path/method validation and hop-by-hop header coverage.
int main() { return edgedesk::test::finish("backend_proxy_test"); }
