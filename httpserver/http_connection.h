#pragma once

#include <iostream>
#include <map>
#include <memory>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace loki {
class ServiceNode;
}

class http_connection;

namespace http = boost::beast::http; // from <boost/beast/http.hpp>
namespace pt = boost::property_tree; // from <boost/property_tree/>

using request_t = http::request<http::string_body>;

using http_callback_t = std::function<void(std::shared_ptr<std::string>)>;

using rpc_function = std::function<void(const pt::ptree&)>;

namespace loki {

void make_http_request(boost::asio::io_context& ioc, std::string ip,
                       uint16_t port, const request_t& req,
                       http_callback_t cb);

void make_http_request(boost::asio::io_context& ioc, std::string ip,
                       uint16_t port, std::string target, std::string body,
                       http_callback_t cb);

class HttpClientSession
    : public std::enable_shared_from_this<HttpClientSession> {

    using tcp = boost::asio::ip::tcp;

    boost::asio::io_context& ioc_;
    boost::beast::flat_buffer buffer_;
    request_t req_;
    http::response<http::string_body> res_;

    http_callback_t callback_;

    bool used_callback_ = false;

    void on_write(boost::system::error_code ec, std::size_t bytes_transferred);

    void on_read(boost::system::error_code ec, std::size_t bytes_transferred);

    void init_callback(std::shared_ptr<std::string> body);

  public:
    tcp::socket socket_;
    // Resolver and socket require an io_context
    explicit HttpClientSession(boost::asio::io_context& ioc, const request_t& req, http_callback_t cb);

    void on_connect();

    ~HttpClientSession();
};

namespace http_server {

class connection_t : public std::enable_shared_from_this<connection_t> {

    using tcp = boost::asio::ip::tcp;

  private:
    boost::asio::io_context& ioc_;

    // The socket for the currently connected client.
    tcp::socket socket_;

    // The buffer for performing reads.
    boost::beast::flat_buffer buffer_{8192};

    // The request message.
    http::request<http::dynamic_body> request_;

    // The response message.
    http::response<http::dynamic_body> response_;

    /// TODO: move these if possible
    std::map<std::string, std::string> header_;
    std::map<std::string, rpc_function> rpc_endpoints_;

    // The timer for putting a deadline on connection processing.
    boost::asio::basic_waitable_timer<std::chrono::steady_clock> deadline_;

    ServiceNode& service_node_;

  public:
    connection_t(boost::asio::io_context& ioc, tcp::socket socket,
                 ServiceNode& sn);

    ~connection_t();

    /// Initiate the asynchronous operations associated with the connection.
    void start();

  private:
    /// Asynchronously receive a complete request message.
    void read_request();

    /// Determine what needs to be done with the request message
    /// (synchronously).
    void process_request();

    /// Asynchronously transmit the response message.
    void write_response();

    /// Syncronously (?) process client store/load requests
    void process_client_req();

    // Check whether we have spent enough time on this connection.
    void register_deadline();

    /// TODO: should move somewhere else
    template <typename T> bool parse_header(T key_list);
};

void run(boost::asio::io_context& ioc, const char* address, uint16_t port,
         ServiceNode& swarm);

} // namespace http_server

} // namespace loki