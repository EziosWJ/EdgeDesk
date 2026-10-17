#include "app/application.hpp"
#include "app/recovery.hpp"
#include "core/config.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        auto config = edgedesk::parse_args(argc, const_cast<const char* const*>(argv));
        if (config.help_requested) {
            std::cout << edgedesk::help_text();
            return 0;
        }
        if (config.version_requested) {
            std::cout << "EdgeDesk " EDGEDESK_VERSION << std::endl;
            return 0;
        }
        if (config.reset_admin_password) {
            return edgedesk::run_reset_admin_password(config);
        }
        edgedesk::Application application(std::move(config));
        return application.run();
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
