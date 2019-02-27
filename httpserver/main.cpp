#include "http_connection.h"
#include "swarm.h"
#include "Database.hpp"

#include "service_node.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <thread>
#include <iostream>
#include <boost/log/trivial.hpp>

using namespace service_node;

int main(int argc, char* argv[]) {

    try {
        // Check command line arguments.
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " <address> <port>\n";
            std::cerr << "  For IPv4, try:\n";
            std::cerr << "    receiver 0.0.0.0 80\n";
            std::cerr << "  For IPv6, try:\n";
            std::cerr << "    receiver 0::0 80\n";
            return EXIT_FAILURE;
        }
        std::cout << "Listening at address " << argv[1] << " port "
                  << argv[2] << std::endl;

        const auto port = static_cast<uint16_t>(std::atoi(argv[2]));

        boost::asio::io_context ioc{1};

        loki::ServiceNode service_node(ioc, argv[2]);

        /// Should run http server
        loki::http_server::run(ioc, argv[1], port, service_node);

    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "Exception caught in main: " << e.what();
        return EXIT_FAILURE;
    }
}
