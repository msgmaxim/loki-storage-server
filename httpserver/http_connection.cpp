#include "Database.hpp"
#include "pow.hpp"
#include "utils.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <thread>

#include <boost/log/trivial.hpp>

#include "http_connection.h"
#include "service_node.h"

using tcp = boost::asio::ip::tcp;    // from <boost/asio.hpp>
namespace http = boost::beast::http; // from <boost/beast/http.hpp>
namespace pt = boost::property_tree; // from <boost/property_tree/>
using namespace service_node;

/// +===========================================

namespace loki {

void make_http_request(boost::asio::io_context& ioc, std::string ip,
                       uint16_t port, const request_t& req,
                       http_callback_t cb) {

    boost::system::error_code ec;

    boost::asio::ip::address ip_address =
        boost::asio::ip::address::from_string(ip, ec);

    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Failed to parse the IP address. Error code = "
                  << ec.value() << ". Message: " << ec.message();
        return;
    }

    boost::asio::ip::tcp::endpoint ep(ip_address, port);

    if (req.target() == "/v1/swarms/push") {
        assert(req.find("X-Loki-recipient") != req.end());
    }

    auto session = std::make_shared<HttpClientSession>(ioc, req, cb);

    session->socket_.async_connect(
        ep, [=](const boost::system::error_code& ec) {
            /// TODO: I think I should just call again if ec == EINTR
            if (ec) {
                BOOST_LOG_TRIVIAL(error) << boost::format("Could not connect to %1%:%2%, message: %3% (%4%)") % ip % port % ec.message() % ec.value();
                /// TODO: handle error better here
                return;
            }

            session->on_connect();
        });
}

void make_http_request(boost::asio::io_context& ioc, std::string ip,
                       uint16_t port, std::string target, std::string body,
                       http_callback_t cb) {

    request_t req;

    req.body() = body;
    req.target(target);

    make_http_request(ioc, ip, port, req, cb);
}

namespace http_server {

using error_code = boost::system::error_code;

static void log_error(const error_code& ec) {
    std::cerr << boost::format("Error(%1%): %2%\n") % ec.value() % ec.message();
}

// "Loop" forever accepting new connections.
static void accept_connection(boost::asio::io_context& ioc,
                              tcp::acceptor& acceptor, tcp::socket& socket,
                              ServiceNode& sn) {

    acceptor.async_accept(socket, [&](const error_code& ec) {

        BOOST_LOG_TRIVIAL(trace) << "connection accepted";
        if (!ec)
            std::make_shared<connection_t>(ioc, std::move(socket), sn)->start();

        if (ec)
            log_error(ec);

        accept_connection(ioc, acceptor, socket, sn);
    });
}

void run(boost::asio::io_context& ioc, const char* addr_str, uint16_t port,
         ServiceNode& sn) {

    BOOST_LOG_TRIVIAL(trace) << "http server run";

    const auto address =
        boost::asio::ip::make_address(addr_str); /// throws if incorrect

    boost::asio::ip::tcp::acceptor acceptor{ioc, {address, port}};
    boost::asio::ip::tcp::socket socket{ioc};

    accept_connection(ioc, acceptor, socket, sn);

    ioc.run();
}

/// TODO: is this inefficient?
static std::string collect_body(http::request<http::dynamic_body>& req) {

    std::string bytes;

    for (auto seq : req.body().data()) {
        const auto* cbuf = boost::asio::buffer_cast<const char*>(seq);
        bytes.insert(std::end(bytes), cbuf,
                     cbuf + boost::asio::buffer_size(seq));
    }

    return bytes;
}

/// ============ connection_t ============

connection_t::connection_t(boost::asio::io_context& ioc, tcp::socket socket,
                           ServiceNode& sn)
    : ioc_(ioc), socket_(std::move(socket)), service_node_(sn),
      deadline_(ioc, std::chrono::seconds(60)) {

    BOOST_LOG_TRIVIAL(trace) << "connection_t";
    /// NOTE: I'm not sure if the timer is working properly
}

connection_t::~connection_t() {
    BOOST_LOG_TRIVIAL(trace) << "~connection_t";
}

void connection_t::start() {

    // assign_callbacks();
    register_deadline();
    read_request();
}

// Asynchronously receive a complete request message.
void connection_t::read_request() {

    auto self = shared_from_this();

    auto on_data = [self](error_code ec, size_t bytes_transferred) {

        BOOST_LOG_TRIVIAL(trace) << "on data";

        boost::ignore_unused(bytes_transferred);

        if (ec) {
            log_error(ec);
            return;
        }

        // NOTE: this is blocking, we should make this asyncronous
        try {
            self->process_request();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Exception caught: " << e.what();
        }

        self->write_response();

    };

    http::async_read(socket_, buffer_, request_, on_data);


}

// Determine what needs to be done with the request message.
void connection_t::process_request() {

    /// This method is responsible for filling out response_

    BOOST_LOG_TRIVIAL(trace) << "process request";
    response_.version(request_.version());
    response_.keep_alive(false);

    /// TODO: make sure that we always send a response!

    response_.result(http::status::bad_request);

    const auto target = request_.target();
    switch (request_.method()) {
    case http::verb::post:
        if (target == "/v1/storage_rpc") {
            /// Store/load from clients
            BOOST_LOG_TRIVIAL(trace) << "got /v1/storage_rpc";

            try {
                process_client_req();
            } catch (std::exception& e) {
                response_.result(http::status::internal_server_error);
                BOOST_LOG_TRIVIAL(trace) << "exception caught while processing client request: " << e.what();
            }

        /// Make sure only service nodes can use this API
        } else if (target == "/v1/swarms/push") {

            BOOST_LOG_TRIVIAL(trace) << "swarms/push";

            const std::vector<std::string> keys = {"X-Loki-recipient"};

            parse_header(keys);

            BOOST_LOG_TRIVIAL(trace) << "got PK: " << header_["X-Loki-recipient"];

            std::string text = collect_body(request_);

            auto pk = header_["X-Loki-recipient"];

            auto msg = std::make_shared<message_t>(pk.c_str(), text.c_str());

            /// TODO: this will need to be done asyncronoulsy
            service_node_.process_push(msg);

            response_.result(http::status::ok);
        } else if (target == "/retrieve_all") {
            boost::beast::ostream(response_.body()) << service_node_.get_all_messages();
            response_.result(http::status::ok);
        } else if (target == "/v1/swarms/push_all") {
            response_.result(http::status::ok);

            std::string body = collect_body(request_);

            // TODO: investigate whether I need a string here
            service_node_.process_push_all(std::make_shared<std::string>(body));

        } else if (target == "/test") {
            // response_.body() = "all good!";
            response_.result(http::status::ok);
            boost::beast::ostream(response_.body()) << "all good!";
        } else if (target == "/quit") {
            BOOST_LOG_TRIVIAL(trace) << "got /quit request";
            ioc_.stop();
            // exit(0);
        } else if (target == "/purge") {
            BOOST_LOG_TRIVIAL(trace) << "got /purge request";
            service_node_.purge_outdated();
        } else {
            BOOST_LOG_TRIVIAL(error) << "unknown target: " << target;
            response_.result(http::status::not_found);
        }
        break;
    case http::verb::get:
        BOOST_LOG_TRIVIAL(error) << "GET requests not supported";
        response_.result(http::status::bad_request);
        break;
    default:
        BOOST_LOG_TRIVIAL(error) << "bad request";
        response_.result(http::status::bad_request);
        break;
    }
}

// Asynchronously transmit the response message.
void connection_t::write_response() {

    response_.set(http::field::content_length, response_.body().size());

    auto self = shared_from_this();

    /// This attempts to write all data to a stream
    /// TODO: handle the case when we are trying to send too much
    http::async_write(socket_, response_, [self](error_code ec, size_t) {
        self->socket_.shutdown(tcp::socket::shutdown_send, ec);
        self->deadline_.cancel();
    });
}

template <typename T> bool connection_t::parse_header(T key_list) {
    for (const auto key : key_list) {
        const auto it = request_.find(key);
        if (it == request_.end()) {
            response_.result(http::status::bad_request);
            response_.set(http::field::content_type, "text/plain");
            boost::beast::ostream(response_.body())
                << "Missing field in header : " << key;

            BOOST_LOG_TRIVIAL(error) << "Missing field in header : " << key;
            return false;
        }
        header_[key] = it->value().to_string();
    }
    return true;
}

void connection_t::process_client_req() {

    const std::vector<std::string> keys = {"X-Loki-EphemKey",
                                           "X-Loki-recipient"};
    if (!parse_header(keys)) {
        BOOST_LOG_TRIVIAL(error) << "could not parse headers\n";
        return;
    }

    auto pk = header_["X-Loki-recipient"];
    BOOST_LOG_TRIVIAL(trace) << "PK: " << pk << std::endl;

    const std::string plainText = collect_body(request_);
    BOOST_LOG_TRIVIAL(trace) << boost::format("message body:<%1%>") % plainText;

    // parse json
    pt::ptree root;
    std::stringstream ss;
    ss << plainText;

    /// TODO: this may throw, need to handle
    pt::json_parser::read_json(ss, root);

    const auto method_name = root.get("method", "");

    if (method_name == "store") {
        //// ==== PROCESS STORE ====

        const boost::property_tree::ptree& body = root.get_child("args");

        const auto data = body.get_child("data").get_value<std::string>();

        BOOST_LOG_TRIVIAL(trace) << "store body: " << data;
        auto msg = std::make_shared<message_t>(pk.c_str(), data.c_str());
        if (service_node_.process_store(msg)) {
            response_.result(http::status::ok);
        } else {
            BOOST_LOG_TRIVIAL(trace) << "BAD store request: " << data;
            response_.result(http::status::bad_request);
        }
        //// ==== END ====
    } else if (method_name == "retrieve") {

        boost::beast::ostream(response_.body()) << service_node_.get_all_messages(pk);
        response_.result(http::status::ok);
    } else {
        response_.result(http::status::bad_request);
        boost::beast::ostream(response_.body()) << "no method: " << method_name;
    }

}

void connection_t::register_deadline() {

    auto self = shared_from_this();

    deadline_.async_wait([self](error_code ec) {
        if (ec) {

            if (ec != boost::asio::error::operation_aborted) {
                log_error(ec);
            }

        } else {

            BOOST_LOG_TRIVIAL(error) << "socket timed out";
            // Close socket to cancel any outstanding operation.
            self->socket_.close(ec);
        }
    });
}

/// ============

} // namespace http_server

/// TODO: make generic, avoid message copy
HttpClientSession::HttpClientSession(boost::asio::io_context& ioc,
                                     const request_t& req, http_callback_t cb)
    : ioc_(ioc), socket_(ioc), callback_(cb) {

    if (req.target() == "/v1/swarms/push") {
        assert(req.find("X-Loki-recipient") != req.end());

        req_.set("X-Loki-recipient", req.at("X-Loki-recipient"));
    }

    req_.method(http::verb::post);
    req_.version(11);
    req_.target(req.target());
    req_.set(http::field::host, "localhost");
    req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req_.body() = req.body();
    req_.prepare_payload();
}

void HttpClientSession::on_connect() {

    BOOST_LOG_TRIVIAL(trace) << "on connect";
    http::async_write(socket_, req_,
                      std::bind(&HttpClientSession::on_write,
                                shared_from_this(), std::placeholders::_1,
                                std::placeholders::_2));
}

void HttpClientSession::on_write(boost::system::error_code ec,
                                 std::size_t bytes_transferred) {

    BOOST_LOG_TRIVIAL(trace) << "on write";
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Error on write, ec: " << ec.value()
                  << ". Message: " << ec.message();
        return;
    }

    BOOST_LOG_TRIVIAL(trace) << "Successfully transferred " << bytes_transferred << " bytes";

    // Receive the HTTP response
    http::async_read(socket_, buffer_, res_,
                     std::bind(&HttpClientSession::on_read, shared_from_this(),
                               std::placeholders::_1, std::placeholders::_2));
}

void HttpClientSession::on_read(boost::system::error_code ec,
                                std::size_t bytes_transferred) {

    BOOST_LOG_TRIVIAL(trace) << "Successfully received " << bytes_transferred << " bytes";

    std::shared_ptr<std::string> body = nullptr;

    if (!ec || (ec == http::error::end_of_stream)) {

        if (http::to_status_class(res_.result_int()) == http::status_class::successful) {
            body = std::make_shared<std::string>(res_.body());
        }

    } else {
        BOOST_LOG_TRIVIAL(error) << "Error on read: " << ec.value() 
            << ". Message: " << ec.message();
    }

    // Gracefully close the socket
    socket_.shutdown(tcp::socket::shutdown_both, ec);

    // not_connected happens sometimes so don't bother reporting it.
    if (ec && ec != boost::system::errc::not_connected) {

        BOOST_LOG_TRIVIAL(error) << "ec: " << ec.value() << ". Message: " << ec.message();
        return;
    }

    init_callback(body);

    // If we get here then the connection is closed gracefully
}

void HttpClientSession::init_callback(std::shared_ptr<std::string> body) {

    ioc_.post(std::bind(callback_, body));
    used_callback_ = true;
}

/// We execute callback (if haven't already) here to make sure it is called
HttpClientSession::~HttpClientSession() {

    if (!used_callback_) {
        ioc_.post(std::bind(callback_, nullptr));
    }
}

} // namespace loki
