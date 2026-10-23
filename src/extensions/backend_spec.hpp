#pragma once

#include <string>
#include <vector>

// Optional native backend declaration from manifest.json (SPEC-0011 section
// 4.2). The manifest may only name the in-package executable and its argv; it
// can never select an identity, a socket path or a data directory. Those are
// administrator/host properties stored outside the package.
namespace edgedesk::extensions {

struct BackendSpec {
    bool present = false;
    // Normalized path relative to the package root. Rejected unless it names a
    // regular, executable ELF file inside the revision.
    std::string executable;
    // Each element is one argv entry; no shell expansion is ever performed.
    std::vector<std::string> args;
};

} // namespace edgedesk::extensions
