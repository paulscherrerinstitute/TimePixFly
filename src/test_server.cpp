// Provide tpx3app test server

#include <iostream>
#include <functional>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/Util/Application.h>
#include <Poco/URI.h>

using Poco::Net::HTTPServerResponse;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServerParams;
using Poco::Net::ServerSocket;
using Poco::Net::StreamSocket;
using Poco::Net::SocketAddress;
using Poco::Util::Application;
using Poco::JSON::Parser;
using Poco::JSON::Object;
using Poco::JSON::Array;
using Poco::URI;
using Poco::RuntimeException;
using Poco::DataFormatException;

namespace {
    std::map<std::string, std::function<void(HTTPServerRequest&, HTTPServerResponse&)>> path_handler;
    SocketAddress destination;
    std::atomic<bool> stop_server = false;
    std::mutex stop_mutex;
    std::condition_variable stop_condition;
    std::thread data_sender;
    unsigned number_of_chips = 4;

    void send_data()
    {
        try {
            std::cout << "send data thread started ...\n";
            StreamSocket con(destination);
            con.close();
            std::cout << "send data thread stopped.\n";
        } catch (std::exception& ex) {
            std::cerr << "data sender: " << ex.what() << '\n';
        } catch (...) {
            std::cerr << "data sender: undefined error\n";
        }
    }

    template<typename T>
    T check_ptr(T&& ptr, const std::string& msg)
    {
        if (ptr == nullptr)
            throw DataFormatException(msg);
        return ptr;
    }

    void error_response(HTTPServerResponse& response, const std::string& msg)
    {
        response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
        response.send() << msg << '\n';
    }

    struct GetRequestHandler final : public HTTPRequestHandler {
        void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
        {
            std::string path = URI{request.getURI()}.getPath();
            std::cout << "GET: " << path << '\n';
            auto handler = path_handler.find(path);
            if (handler == std::end(path_handler))
                error_response(response, "GET: unsupported path");
            else
                handler->second(request, response);
        }
    };

    struct PutRequestHandler final : public HTTPRequestHandler {
        void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
        {
            std::string path = URI{request.getURI()}.getPath();
            std::cout << "PUT: " << path << '\n';
            if (request.getContentType() != "application/json") {
                error_response(response, "Only json content is accepted.");
                return;
            }
            auto handler = path_handler.find(path);
            if (handler == std::end(path_handler))
                error_response(response, "PUT: unsupported path");
            else
                handler->second(request, response);
        }
    };
    
    struct ErrorRequestHandler final : public HTTPRequestHandler {
        void handleRequest([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response) override
        {
            error_response(response, error);
        }

        explicit ErrorRequestHandler(const std::string err)
            : error(err)
        {}

        virtual ~ErrorRequestHandler() = default;

        std::string error;
    };

    struct TestServerRequestHandlerFactory  final : public HTTPRequestHandlerFactory {
        HTTPRequestHandler * createRequestHandler(const HTTPServerRequest & request) override
        {

            if (request.getMethod() == "GET")
                return new GetRequestHandler{};
            else if (request.getMethod() == "PUT")
                return new PutRequestHandler{};
            else
                return new ErrorRequestHandler{"Only GET and PUT methods are supported."};
        }
    };

    void get_dashboard([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        response.setContentType("application/json");
        response.send() << R"({"Server":{"SoftwareVersion":"t1"}})" << '\n';
    }

    void get_config_load(HTTPServerRequest& request, HTTPServerResponse& response)
    {
        try {
            auto uri = URI{request.getURI()};
            std::cout << uri.toString() << '\n';
            std::unordered_map<std::string, std::string> query;
            for (const auto& part : uri.getQueryParameters()) {
                std::cout << part.first << '=' << part.second << '\n';
                query.insert(part);
            }
            response.setContentType("text/plain");
            response.send() << "config load " << query["format"] << '=' << query["file"] << " - ignored\n";
        } catch (Poco::Exception& ex) {
            error_response(response, ex.displayText());
        } catch (std::exception& ex) {
            error_response(response, ex.what());
        }
    }
    
    void get_measurement_start([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        data_sender = std::thread(send_data);
        response.setContentType("text/plain");
        response.send() << "measurement started\n";
    }

    void getput_detector_config(HTTPServerRequest& request, HTTPServerResponse& response)
    {
        if (request.getMethod() == "GET") {
            response.setContentType("application/json");
            response.send() << R"({"conf":"dummy"})" << '\n';
        } else {
            auto json_data = Parser{}.parse(request.stream());
            std::cout << json_data.toString() << '\n';
            response.setContentType("text/plain");
            response.send() << "detector config - ignored\n";
        }
    }

    void get_detector_info([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        response.setContentType("application/json");
        response.send() << R"({"NumberOfChips":)" << number_of_chips << "}\n";
    }

    void get_stop([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        std::lock_guard lock(stop_mutex);
        stop_server.store(true, std::memory_order_release);
        stop_condition.notify_one();
        response.setContentType("text/plain");
        response.send() << "server stop\n";
    }

    void put_server_destination(HTTPServerRequest& request, HTTPServerResponse& response)
    {
        try {
            auto json_data = Parser{}.parse(request.stream());
            std::cout << json_data.toString() << '\n';
            auto json_object = json_data.extract<Object::Ptr>();
            auto json_array = check_ptr(json_object->getArray("Raw"), "expected 'Raw' array");
            auto json_value = check_ptr(json_array->getObject(0), "expected object as element 0");
            auto connect_to = URI{json_value->getValue<std::string>("Base")};
            if (connect_to.getScheme() != "tcp")
                throw RuntimeException("expected tcp as scheme");
            if (connect_to.getUserInfo() != "connect")
                throw RuntimeException("expected connect as userinfo");
            destination = SocketAddress(connect_to.getHost(), connect_to.getPort());

            response.setContentType("text/plain");
            response.send() << "server dest - " << destination.toString() << '\n';
        } catch (Poco::Exception& ex) {
            error_response(response, ex.displayText());
        } catch (std::exception& ex) {
            error_response(response, ex.what());
        }
    }

    void init_handlers()
    {
        path_handler.emplace("/dashboard", get_dashboard);
        path_handler.emplace("/config/load", get_config_load);
        path_handler.emplace("/measurement/start", get_measurement_start);
        path_handler.emplace("/detector/config", getput_detector_config);
        path_handler.emplace("/detector/info", get_detector_info);
        path_handler.emplace("/stop", get_stop);

        path_handler.emplace("/server/destination", put_server_destination);
    }

    void handle_args(int argc, char *argv[])
    {
        enum { ready, file_name } state = ready;
        for (int i=1; i<argc; i++) {
            std::string arg{argv[i]};
            if ((statearg == "-f")
                state = 1;
        }
    }
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char *argv[])
{
    handle_args(argc, argv);
    init_handlers();

    {
        struct Params final : public HTTPServerParams {
            ~Params() override
            {}
        };
        std::unique_ptr<Params> server_params(new Params{});
        ServerSocket socket(SocketAddress{"localhost:8080"});
        server_params->setMaxThreads(1);
        HTTPServer server{new TestServerRequestHandlerFactory{}, socket, server_params.release()};
        std::cout << "starting server on " << socket.address().toString() << " ...\n";
        server.start();
        {
            std::unique_lock lock(stop_mutex);
            while (! stop_server.load(std::memory_order_acquire))
                stop_condition.wait(lock);
        }
        if (data_sender.joinable()) {
            std::cout << "joining sender thread ...\n";
            data_sender.join();
        }
        server.stop();
        std::cout << "server stopped.\n";
    }

    return Application::EXIT_OK;
}