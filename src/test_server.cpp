/*!
\file
Test server code for replaying an ASI raw event stream
*/

#include <iostream>
#include <sstream>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <cmath>
#include <condition_variable>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/SocketStream.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/Util/Application.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/OptionProcessor.h>
#include <Poco/Util/OptionException.h>
#include <Poco/URI.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>

using Poco::Net::HTTPServerResponse;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServerParams;
using Poco::Net::ServerSocket;
using Poco::Net::StreamSocket;
using Poco::Net::SocketOutputStream;
using Poco::Net::SocketAddress;
using Poco::Util::Application;
using Poco::Util::Option;
using Poco::Util::OptionCallback;
using Poco::Util::OptionSet;
using Poco::Util::OptionProcessor;
using Poco::Util::HelpFormatter;
using Poco::JSON::Parser;
using Poco::JSON::Object;
using Poco::JSON::Array;
using Poco::URI;
using Poco::FileInputStream;
using Poco::StreamCopier;
using Poco::RuntimeException;
using Poco::DataFormatException;
using Poco::InvalidArgumentException;
using Poco::Util::UnknownOptionException;

namespace {
    using namespace std::chrono_literals;

    std::map<std::string, std::function<void(HTTPServerRequest&, HTTPServerResponse&)>> path_handler; //!< Map HTTP from path to handler
    ServerSocket bind_to{SocketAddress{"localhost:8080"}}; //!< Server binding address
    SocketAddress destination;                  //!< Destination address
    OptionSet args;                             //!< Commandline arguments OptionSet
    bool stop_server = false;                   //!< Signal for stop server
    std::mutex stop_mutex;                      //!< Protection for stop signal
    std::condition_variable stop_condition;     //!< Condition variable for stop signal
    bool sender_ready = false;                  //!< Sender thread ready signal
    std::mutex ready_mutex;                     //!< Protection for sender ready signal
    std::condition_variable ready_condition;    //!< Condition variable for ready signal
    std::thread data_sender;                    //!< Data sender thread
    std::string file_name;                      //!< Raw data stream file name
    unsigned number_of_chips = 4;               //!< Default value for number of detector chips

    /*!
    \brief Code for send data thread
    */
    void send_data()
    {
        try {
            std::cout << "send data thread started ...\n";
            {
                StreamSocket con(destination);
                SocketOutputStream output_stream(con);
                FileInputStream data_file{file_name};
                {
                    std::lock_guard lock(ready_mutex);
                    sender_ready = true;
                    ready_condition.notify_one();
                }
                StreamCopier::copyStream(data_file, output_stream);
                //std::this_thread::sleep_for(5s);
            }
            std::cout << "send data thread stopped.\n";
        } catch (std::exception& ex) {
            std::cerr << "data sender: " << ex.what() << '\n';
        } catch (...) {
            std::cerr << "data sender: undefined error\n";
        }
        {
            std::lock_guard lock(stop_mutex);
            stop_server = true;
            stop_condition.notify_one();
        }
    }

    /*!
    \brief Null pointer check
    \param ptr Pointer to check
    \param msg Error message
    \return ptr if it is not null
    \throw DataFormatException if ptr is null
    */
    template<typename T>
    T check_ptr(T&& ptr, const std::string& msg)
    {
        if (ptr == nullptr)
            throw DataFormatException(msg);
        return ptr;
    }

    /*!
    \brief Send HTTP BAD_REQUEST response
    \param response Poco HTTP response object
    \param msg      Error message
    */
    void error_response(HTTPServerResponse& response, const std::string& msg)
    {
        response.setStatus(HTTPResponse::HTTP_BAD_REQUEST);
        response.send() << msg << '\n';
    }

    /*!
    \brief HTTP GET request handler
    */
    struct GetRequestHandler final : public HTTPRequestHandler {
        /*!
        \brief Handler function
        \param request  Poco HTTP request object reference
        \param response Poco HTTP response object
        */
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

    /*!
    \brief HTTP PUT request handler
    */
    struct PutRequestHandler final : public HTTPRequestHandler {
        /*!
        \brief Handler function
        \param request  Poco HTTP request object reference
        \param response Poco HTTP response object
        */
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
    
    /*!
    \brief HTTP unknown request type handler
    */
    struct ErrorRequestHandler final : public HTTPRequestHandler {
        /*!
        \brief Handler function
        \param request  Poco HTTP request object reference
        \param response Poco HTTP response object
        */
        void handleRequest([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response) override
        {
            error_response(response, error);
        }

        /*!
        \brief Constructor
        \param err  Error string
        */
        explicit ErrorRequestHandler(const std::string err)
            : error(err)
        {}

        virtual ~ErrorRequestHandler() = default;

        std::string error;  //!< Error string
    };

    /*!
    \brief Factory generating HTTP request specific handlers
    */
    struct TestServerRequestHandlerFactory  final : public HTTPRequestHandlerFactory {
        /*!
        \brief Create a request handler depending on request method
        \param request Poco HTPP request object reference
        \return Pointer to Poco request handler object
        */
        HTTPRequestHandler* createRequestHandler(const HTTPServerRequest & request) override
        {

            if (request.getMethod() == "GET")
                return new GetRequestHandler{};
            else if (request.getMethod() == "PUT")
                return new PutRequestHandler{};
            else
                return new ErrorRequestHandler{"Only GET and PUT methods are supported."};
        }
    };

    /*!
    \brief GET /dashboard response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
    void get_dashboard([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        response.setContentType("application/json");
        response.send() << R"({"Server":{"SoftwareVersion":"t1"}})" << '\n';
    }

    /*!
    \brief GET /config/load response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
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
    
    /*!
    \brief GET /mesurement/start response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
    void get_measurement_start([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        data_sender = std::thread(send_data);
        {
            std::unique_lock lock(ready_mutex);
            while (! sender_ready)
                ready_condition.wait(lock);
        }
        response.setContentType("text/plain");
        response.send() << "measurement started\n";
    }

    /*!
    \brief GET/PUT /detector/config response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
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

    /*!
    \brief GET /detector/info response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
    void get_detector_info([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        response.setContentType("application/json");
        response.send() << R"({"NumberOfChips":)" << number_of_chips << "}\n";
    }

    /*!
    \brief GET /detector/layout response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
    void get_detector_layout([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        auto width = static_cast<unsigned>(std::ceil(std::sqrt(number_of_chips)));
        auto height = number_of_chips / width;
        if ((width * height) != number_of_chips)
            throw InvalidArgumentException("number of chips argument cannot be decomposed properly into width and height");

        std::ostringstream oss;
        oss << R"({"Original":{"Width":)" << width * 256
            << R"(,"Height":)" << height * 256 << R"(,"Chips":[)";
        
        unsigned i=1;
        for (unsigned h=0; h<height; h++) {
            auto posX = h * 256;
            for(unsigned w=0; w<width; i++, w++) {
                auto posY = w * 256;
                oss << R"({"X":)" << posX << R"(,"Y":)" << posY << "}";
                if (i < number_of_chips)
                    oss << ',';
            }
        }
        oss << "]}}\n";
        
        response.setContentType("application/json");
        response.send() << oss.str();
    }

    /*!
    \brief GET /stop response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
    void get_stop([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
    {
        {
            std::lock_guard lock(stop_mutex);
            stop_server = true;
            stop_condition.notify_one();
        }
        response.setContentType("text/plain");
        response.send() << "server stop\n";
    }

    /*!
    \brief PUT /server/destination response
    \param request  Poco HTTP request object
    \param response Poco HTTP response object
    */
    void put_server_destination(HTTPServerRequest& request, HTTPServerResponse& response)
    {
        try {
            auto json_data = Parser{}.parse(request.stream());
            std::cout << json_data.toString() << '\n';
            auto json_object = json_data.extract<Object::Ptr>();
            auto json_array = check_ptr(json_object->getArray("Raw").get(), "expected 'Raw' array");
            auto json_value = check_ptr(json_array->getObject(0).get(), "expected object as element 0");
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

    /*!
    \brief Initialize HTTP handlers
    */
    void init_handlers()
    {
        path_handler.emplace("/dashboard", get_dashboard);
        path_handler.emplace("/config/load", get_config_load);
        path_handler.emplace("/measurement/start", get_measurement_start);
        path_handler.emplace("/detector/config", getput_detector_config);
        path_handler.emplace("/detector/info", get_detector_info);
        path_handler.emplace("/detector/layout", get_detector_layout);
        path_handler.emplace("/stop", get_stop);

        path_handler.emplace("/server/destination", put_server_destination);
    }

    /*!
    \brief Commandline options handler type
    */
    struct option_handler_type final {
        /*!
        \brief Help option handler
        \param name  Option name
        \param value Option value
        */
        inline void handle_help([[maybe_unused]] const std::string& name, [[maybe_unused]] const std::string& value)
        {
            HelpFormatter helpFormatter(args);
            helpFormatter.setCommand("server");
            helpFormatter.setUsage("OPTIONS");
            helpFormatter.setHeader("Simulate raw stream from raw events input file.");
            helpFormatter.format(std::cout);
            std::exit(Application::EXIT_OK);
        }

        /*!
        \brief String valued option handler
        \param name  Option name
        \param value Option value
        */
        inline void handle_string(const std::string& name, const std::string& value)
        {
            if (name == "input")
                file_name = value;
            else if (name == "bind")
                bind_to = ServerSocket{SocketAddress{value}};
        }

         /*!
        \brief Integer values option handler
        \param name  Option name
        \param value Option value
        */
       inline void handle_number(const std::string& name, const std::string& value)
        {
            long num = stol(value);
            if (name == "nchips")
                number_of_chips = static_cast<unsigned>(num);
        }
    } option_handler;   //!< Commandline options handler object

    /*!
    \brief Handle commandline arguments
    \param argc Number of commandline arguments
    \param argv Commandline argument values
    */
    void handle_args(int argc, char *argv[])
    {
        args.addOption(Option{"input", "i"}
            .description("raw events input file")
            .repeatable(false)
            .argument("FNAME")
            .callback(OptionCallback<option_handler_type>{&option_handler, &option_handler_type::handle_string}));
        args.addOption(Option{"bind", "b"}
            .description("bind to address")
            .repeatable(false)
            .argument("HOST:PORT")
            .callback(OptionCallback<option_handler_type>{&option_handler, &option_handler_type::handle_string}));
        args.addOption(Option{"nchips", "c"}
            .description("number of chips")
            .repeatable(false)
            .argument("N")
            .callback(OptionCallback<option_handler_type>{&option_handler, &option_handler_type::handle_number}));
        args.addOption(Option{"help", "h"}
            .description("show this help")
            .callback(OptionCallback<option_handler_type>{&option_handler, &option_handler_type::handle_help}));

        OptionProcessor handler(args);

        std::string name, value;
        for (int i=1; i<argc; i++) {
            if (! handler.process(argv[i], name, value))
                throw UnknownOptionException(argv[i]);
            args.getOption(name).callback()->invoke(name, value);
        }
        handler.checkRequired();
    }
}

/*!
\brief Main function
\param argc Number of commandline arguments
\param argv Commandline argument values
\return 0 if no errors, not 0 otherwise
*/
int main(int argc, char *argv[])
{
    try {
        handle_args(argc, argv);
        init_handlers();

        {
            struct Params final : public HTTPServerParams {
                ~Params() override
                {}
            };
            std::unique_ptr<Params> server_params(new Params{});
            server_params->setMaxThreads(1);
            HTTPServer server{new TestServerRequestHandlerFactory{}, bind_to, server_params.release()};
            std::cout << "starting server on " << bind_to.address().toString() << " ...\n";
            server.start();
            {
                std::unique_lock lock(stop_mutex);
                while (! stop_server)
                    stop_condition.wait(lock);
            }
            if (data_sender.joinable()) {
                std::cout << "joining sender thread ...\n";
                data_sender.join();
            }
            server.stop();
            std::cout << "server stopped.\n";
        }
    } catch (Poco::Exception& ex) {
        std::cerr << "Error: " << ex.displayText() << '\n';
        return Application::EXIT_USAGE;
    } catch (std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return Application::EXIT_USAGE;
    }

    return Application::EXIT_OK;
}
