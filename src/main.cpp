#include "app/config.hpp"
#include "app/http_server.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        const auto config = edgedesk::parse_args(argc, const_cast<const char* const*>(argv));
        return edgedesk::run_server(config);
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
