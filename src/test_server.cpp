/*!
\file
Test server code for replaying an ASI raw event stream

TODO:
- don't end server loop after start
- make stop stop data sending
*/

#include <Poco/Util/OptionCallback.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <cerrno>
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
    std::thread data_sender;                    //!< Data sender thread
    std::string file_name;                      //!< Raw data stream file name
    int premature_stall = -1;                   //!< Stall data sending before sending everything
    unsigned number_of_chips = 4;               //!< Default value for number of detector chips
    bool no_data = false;                       //!< Don't send event data

    /*!
    \brief Wrap a boolean signal with extra ops
    */
    struct signal final {
        bool val;                       //!< Signal value
        std::mutex lck;                 //!< Protection lock
        std::condition_variable cond;   //!< Signal condition

        /*!
        \brief Default constructor
        */
        signal() noexcept
            : val{false}
        {}

        /*!
        \brief Construct from bool
        \param value New signal value
        */
        signal(bool value) noexcept
            : val{value}
        {}

        /*!
        \brief Convert to bool
        \return Signal value
        */
        operator bool()
        {
            std::lock_guard lock{lck};
            return val;
        }

        /*!
        \brief Assignment
        \param value New value
        \return self
        */
        signal& operator=(bool value)
        {
            std::lock_guard lock{lck};
            val = value;
            return *this;
        }

        /*!
        \brief Wait for signal value
        \param value Value to wait for
        */
        void await(bool value)
        {
            std::unique_lock lock{lck};
            while (val != value)
                cond.wait(lock);
        }

        /*!
        \brief Wait for signal value and reset it
        \param value Value to wait for
        */
        void await_reset(bool value)
        {
            std::unique_lock lock{lck};
            while (val != value)
                cond.wait(lock);
            val = !val;
        }

        /*!
        \brief Set value and notify one waiter
        \param value New signal value
        */
        void set_notify(bool value)
        {
            std::lock_guard lock{lck};
            val = value;
            cond.notify_one();
        }

        /*!
        \brief Return current signal value and reset it
        \param value New signal value
        \return Signal value
        */
        bool reset(bool value)
        {
            std::lock_guard lock{lck};
            bool rval = val;
            val = value;
            return rval;
        }

        /*!
        \brief Proxy to a locked signal
        */
        struct lock final {
            signal* sig;                        //!< Underlying signal
            std::unique_lock<std::mutex> lck;   //!< Lock on signal

            /*!
            \brief Constructor
            \param s Signal
            */
            explicit lock(signal& s)
                : sig{&s}, lck{s.lck}
            {}

            lock(const lock&) = delete;
            lock& operator=(const lock&) = delete;

            /*!
            \brief Move constructor
            */
            lock(lock&&) = default;

            /*!
            \brief Moving assignment
            \return this
            */
            lock& operator=(lock&&) = default;

            /*!
            \brief Conversion to bool
            \return Underlying signal value
            */
            operator bool()
            {
                return sig->val;
            }

            /*!
            \brief Assignment
            \param value New underlying signal value
            \return this
            */
            lock& operator=(bool value)
            {
                sig->val = value;
                return *this;
            }
        };

        /*!
        \brief Get locked proxy
        \return Proxy to locked signal
        */
        lock sig()
        {
            return lock(*this);
        }
    };


    signal stop_server;     //!< Stop server signal
    signal sender_ready;    //!< Sender is ready signal
    signal start_collect;   //!< Start data collection signal
    signal stop_collect;    //!< Stop data collection signal
    signal break_stall;     //!< Break premature stall

    /*!
    \brief Wrap file descriptor
    */
    struct file_desc final {
        int fd = -1;    //!< File descriptor

        /*!
        \brief Constructor
        \param name File name
        */
        file_desc(const std::string& name)
        {
            fd = open(name.c_str(), O_RDONLY
                #ifdef O_NOATIME
                | O_NOATIME
                #endif
            );
            if (fd < 0)
                throw Poco::RuntimeException(std::string("unable to open file ") + name + ": " + std::strerror(fd));
        }

        file_desc(const file_desc&) = delete;
        file_desc& operator=(const file_desc&) = delete;

        /*!
        \brief Move constructor
        \param other Moved file descriptor wrapper
        */
        file_desc(file_desc&& other) noexcept
        {
            std::swap(fd, other.fd);
        }

        /*!
        \brief Move assignment
        \param other Moved file descriptor wrapper
        \return this
        */
        file_desc& operator=(file_desc&& other) noexcept
        {
            std::swap(fd, other.fd);
            return *this;
        }

        ~file_desc() noexcept
        {
            close(fd);
        }
    };

    /*!
    \brief Wrap mmapped file data
    */
    struct file_data final {
        char* data;     //!< File data pointer
        size_t len;     //!< Data length
        file_desc fd;   //!< File descriptor

        /*!
        \brief Copy constructor
        \param fdesc File descriptor wrapper
        */
        file_data(file_desc&& fdesc)
            : fd{std::move(fdesc)}
        {
            struct stat file_status{};
            if (fstat(fd.fd, &file_status) < 0)
                throw Poco::RuntimeException(std::string("stat failed: ") + std::strerror(errno));
            len = file_status.st_size;
            if (! (data = (char *)mmap(nullptr, len, PROT_READ, MAP_PRIVATE
                #ifdef MAP_POPULATE
                | MAP_POPULATE
                #endif
                , fd.fd, 0)))
                throw Poco::RuntimeException(std::string("mmap failed: ") + std::strerror(errno));
        }

        /*!
        \brief Destructor
        */
        ~file_data() noexcept
        {
            munmap(data, len);
        }
    };

    /*!
    \brief Code for send data thread
    */
    void send_data()
    {
        constexpr static int header_size = 16;  // for premature stall
        try {
            std::cout << "send data thread started ...\n";
            sender_ready.set_notify(true);
            file_data fd{file_desc{file_name}};
            if (fd.len < header_size)
                throw Poco::RuntimeException(std::string("input file is not large enough - ") + file_name);

            do {
                std::cout << "data sender: waiting for start signal...\n";
                do {
                    if (stop_server)
                        goto stop_reader;
                    if (start_collect.reset(false))
                        break;

                    std::this_thread::sleep_for(50ms);
                } while (true);
                {
                    std::cout << "data sender: received start\n";
                    if (premature_stall == 0) {
                        std::cout << "premature stall before connect\n";
                        break_stall.await_reset(true);
                    }
                    StreamSocket con{destination};
                    if (premature_stall == 1) {
                        std::cout << "premature stall after connect\n";
                        break_stall.await_reset(true);
                    }
                    std::cout << "start sending data to " << destination.toString() << '\n';
                    size_t sent{0};
                    if (premature_stall == 2) {
                        int sz = con.sendBytes(&fd.data[sent], header_size);
                        sent += sz;
                        std::cout << "premature stall after sending " << sz << " bytes\n";
                        break_stall.await_reset(true);
                    }
                    if (no_data)
                        continue;
                    while (sent < fd.len) {
                        std::cout << "data sender: trying to send " << (fd.len - sent) << " after " << sent << " bytes\n";
                        int sz = con.sendBytes(&fd.data[sent], fd.len - sent);
                        std::cout << "data sender: sent " << sz << " bytes\n";
                        sent += sz;

                        if (stop_server)
                            goto stop_reader;
                        if (stop_collect.reset(false))
                            break;
                    }
                }
            } while (true);
        stop_reader:
            std::cout << "send data thread stopped.\n";
        } catch (Poco::Exception& ex) {
            std::cerr << "data sender exception: " << ex.displayText() << '\n';
        } catch (std::exception& ex) {
            std::cerr << "data sender exception : " << ex.what() << '\n';
        } catch (...) {
            std::cerr << "data sender undefined error\n";
        }
        stop_server.set_notify(true);
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
        start_collect = true;
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
        stop_server.set_notify(true);
        response.setContentType("text/plain");
        response.send() << "server stop\n";
    }

    /*!
    \brief GET /kill - kill the server immediately
    \param request Poco HTTP request object
    \param response Poco HTTP response object
    */
    void get_kill([[maybe_unused]] HTTPServerRequest& request, [[maybe_unused]] HTTPServerResponse& response)
    {
        std::cout << "kill server\n";
        std::exit(EXIT_SUCCESS);
    }

    /*!
    \brief GET /break-stall - break premature stall
    \param request Poco HTTP request object
    \param response Poco HTTP response object
    */
    void get_break_stall([[maybe_unused]] HTTPServerRequest& request, [[maybe_unused]] HTTPServerResponse& response)
    {
        std::cout << "break stall\n";
        break_stall.set_notify(true);
        response.setContentType("text/plain");
        response.send() << "break stall\n";
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
        path_handler.emplace("/kill", get_kill);
        path_handler.emplace("/break-stall", get_break_stall);

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
            if (name == "nchips") {
                number_of_chips = static_cast<unsigned>(num);
            } else if (name == "premature-stall") {
                if ((num < 0) || (num > 2))
                    throw InvalidArgumentException("invalid premature stall value");
                premature_stall = num;
            }
        }

        inline void handle_bool(const std::string& name, [[maybe_unused]] const std::string& value)
        { 
            if (name == "no-data") {
                no_data = true;
            }
        }

        // /*!
        // \brief Boolean value option handler
        // \param name Option name
        // \param value Option value {"", "1", "true"}
        // */
        // inline void handle_bool(const std::string& name, const std::string& value)
        // {
        //     constexpr static int nvals = 5;
        //     constexpr static const char* vals[nvals] = {"", "1", "true", "0", "false"};
        //     constexpr static int last_true = 2;
        //     bool val;
        //     int i;
        //     for (i=0; (i<nvals) && (vals[i] != value); i++);
        //     if (i >= nvals)
        //         throw Poco::InvalidArgumentException(std::string("illegal value for boolean option ") + name);
        //     val = (i <= last_true);
        //     if (name == "some-name")
        //         variable = val;
        // }
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
        args.addOption(Option{"premature-stall", "s"}
            .description("premature stall of data sending, 0-before connect/1-after connect/2-after header")
            .repeatable(false)
            .argument("S")
            .callback(OptionCallback<option_handler_type>{&option_handler, &option_handler_type::handle_number}));
        args.addOption(Option{"no-data", "d"}
            .description("don't send any data, just connect and deconnect")
            .repeatable(false)
            .callback(OptionCallback<option_handler_type>{&option_handler, &option_handler_type::handle_bool}));
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
            data_sender = std::thread(send_data);
            sender_ready.await(true);
            server.start();
            stop_server.await(true);
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
