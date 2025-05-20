/*!
\file
Code for tpx3app analysis program

Author: hans-christian.stadler@psi.ch
*/

/*
TODO:
- test mode für rest
- server mode with aquisition start removed
*/

// #include <filesystem>
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include <fstream>
#include <chrono>

#include "Poco/Dynamic/Var.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Parser.h"
#include "Poco/Util/Application.h"
#include "Poco/Util/OptionSet.h"
#include "Poco/Util/HelpFormatter.h"
#include "Poco/Net/SocketAddress.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/ServerSocket.h"
#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/HTTPResponse.h"
#include "Poco/Net/HTTPServerParams.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/Net/HTTPServerResponse.h"
#include "Poco/Net/HTTPRequestHandler.h"
#include "Poco/Net/HTTPRequestHandlerFactory.h"
#include "Poco/Net/HTTPServer.h"
#include "Poco/Net/MediaType.h"
#include "Poco/URI.h"
#include "Poco/Process.h"
#include "Poco/Exception.h"

#include "Poco/StreamCopier.h"

#include "logging.h"
#include "decoder.h"
#include "data_handler.h"
#include "copy_handler.h"
#include "layout.h"
#include "processing.h"
#include "global.h"

namespace {
    using namespace std::string_view_literals;
    using namespace std::chrono_literals;
    using Poco::Util::OptionCallback;
    using Poco::Util::OptionSet;
    using Poco::Util::Option;
    using Poco::Util::Application;
    using Poco::Util::HelpFormatter;
    using Poco::Net::SocketAddress;
    using Poco::Net::StreamSocket;
    using Poco::Net::ServerSocket;
    using Poco::Net::HTTPClientSession;
    using Poco::Net::HTTPRequest;
    using Poco::Net::HTTPResponse;
    using Poco::Net::HTTPServerParams;
    using Poco::Net::HTTPServerRequest;
    using Poco::Net::HTTPServerResponse;
    using Poco::Net::HTTPRequestHandler;
    using Poco::Net::HTTPRequestHandlerFactory;
    using Poco::Net::HTTPServer;
    using Poco::Net::MediaType;
    using Poco::URI;
    using Poco::LogicException;
    using Poco::InvalidArgumentException;
    using Poco::RuntimeException;
    using Poco::Dynamic::Var;
    using wall_clock = std::chrono::high_resolution_clock;  //!< Clock type

    #include "version.h"

    //=========================
    // JSON handling functions
    //=========================

    /*!
    \brief Extract object from JSON object
    \param object   JSON object containing another object
    \param name     Name of object to extract
    \return Poco pointer to object, or nullptr if the object is not found
    */
    inline Poco::JSON::Object::Ptr operator|(const Poco::JSON::Object::Ptr& object, const std::string& name)
    {
        Poco::JSON::Object::Ptr objPtr = object->getObject(name);
        if (objPtr.isNull())
            throw RuntimeException(std::string("JSON Object contains no object ") + name, __LINE__);
        return objPtr;
    }

    /*!
    \brief Extract list from JSON object
    \param object   JSON object containing a list
    \param name     Name of list to extract
    \return Poco pointer to list, or nullptr if the list is not found
    */
    inline Poco::JSON::Array::Ptr operator/(const Poco::JSON::Object::Ptr& object, const std::string& name)
    {
        Poco::JSON::Array::Ptr arrayPtr = object->getArray(name);
        if (arrayPtr.isNull())
            throw RuntimeException(std::string("JSON Object contains no array ") + name, __LINE__);
        return arrayPtr;
    }

    /*!
    \brief Extract object from JSON list
    \param array    JSON list containing an object
    \param index    List index of object to extract
    \return Poco pointer to object, or nullptr if the object is not found
    */
    inline Poco::JSON::Object::Ptr operator|(const Poco::JSON::Array::Ptr& array, unsigned index)
    {
        Poco::JSON::Object::Ptr objPtr = array->getObject(index);
        if (objPtr.isNull())
            throw RuntimeException(std::string("JSON Array contains no index ") + std::to_string(index), __LINE__);
        return objPtr;
    }

    //=========================
    // REST server
    //=========================

    /*!
    \brief Server response proxy object for text/plain response
    */
    class ResponseText final {
        HTTPServerResponse& response;   //!< Server response
        std::ostream& out;              //!< Stream for response text

    public:
        /*!
        \brief Construct proxy
        \param response_ Server response
        */
        inline ResponseText(HTTPServerResponse& response_)
            : response(response_), out(response_.send())
        {
            response.setContentType("text/plain");
        }

        /*!
        \brief Text output
        \param val Value to print as text/plain in the response
        \return this
        */
        template<typename T>
        inline ResponseText& operator<<(const T& val)
        {
            out << val;
            return *this;
        }

        /*!
        \brief Set response status code
        \param status Status code
        \return this
        */
        inline ResponseText& operator<<(const HTTPResponse::HTTPStatus status)
        {
            response.setStatus(status);
            return *this;
        }
    };

    /*!
    \brief Handle control commands with a rest interface
    */
    class RestHandler final : public HTTPRequestHandler {
        Logger& logger;                 //!< Logger
        Poco::JSON::Parser jsonParser;  //!< Poco JSON parser

    public:
        /*!
        \brief Construct request handler
        \param logger_ Logger
        */
        RestHandler(Logger& logger_)
            : logger(logger_)
        {}

        /*!
        \brief Handle rest requests
        \param request Rest request
        \param response Rest response
        */
        void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override {
            try {
                URI uri{request.getURI()};
                logger << request.getMethod() << " Request: " << uri.toString() << log_notice;
                std::string key = uri.getPath();
                if (request.getMethod() == Poco::Net::HTTPRequest::HTTP_PUT) {
                    if (request.getContentType() != "application/json")
                        throw Poco::DataFormatException("PUT only allowed with JSON content");
                    jsonParser.reset();
                    Poco::JSON::Object::Ptr result = jsonParser
                        .parse(request.stream())
                        .extract<Poco::JSON::Object::Ptr>();
                    const auto& callbacks = global::instance->put_callbacks;
                    try {
                        std::string res = callbacks.at(key)(result);
                        ResponseText(response) << HTTPResponse::HTTP_OK << res;
                    } catch (std::out_of_range&) {
                        throw Poco::DataFormatException(std::string("illegal path - ") + key);
                    }
                } else if (request.getMethod() == Poco::Net::HTTPRequest::HTTP_GET) {
                    URI::QueryParameters params = uri.getQueryParameters();
                    std::string val;
                    if (params.size() == 1) {
                        const auto& keyval = params[0];
                        key += '?';
                        key += keyval.first;
                        val = keyval.second;
                    } else if (params.size() > 1) {
                        ResponseText(response) << HTTPResponse::HTTP_BAD_REQUEST << "Only one key is allowed per request";
                        return;
                    }
                    const auto& callbacks = global::instance->get_callbacks;
                    try {
                        std::string res = callbacks.at(key)(val);
                        ResponseText(response) << HTTPResponse::HTTP_OK << res;
                    } catch (std::out_of_range&) {
                        throw Poco::DataFormatException(std::string("illegal path/key - ") + key);
                    }
                } else {
                    ResponseText(response) << HTTPResponse::HTTP_METHOD_NOT_ALLOWED << "Unsupported method: " << request.getMethod();
                }
            } catch (Poco::Exception& ex) {
                ResponseText(response) << HTTPResponse::HTTP_BAD_REQUEST << "Unable to handle request: " << ex.displayText();
            } catch (const std::exception& ex) {
                ResponseText(response) << HTTPResponse::HTTP_BAD_REQUEST << "Unable to handle request: " << ex.what();
            }
        }
    };

    /*!
    \brief Factory for creating rest handlers
    */
    class RestHandlerFactory final : public HTTPRequestHandlerFactory {
        Logger& logger; //!< Logger

    public:
        /*!
        \brief Create factory
        \param logger_ Logger
        */
        RestHandlerFactory(Logger& logger_)
            : logger(logger_)
        {}

        /*!
        \brief Create rest handler
        \param request Rest request
        \return Handler for rest requests
        */
        HTTPRequestHandler* createRequestHandler([[maybe_unused]] const HTTPServerRequest& request) override {
            return new RestHandler(logger);
        }
    };

    /*!
    \brief Wrapper for rest request handling HTTP server
    */
    class RestService final {
        HTTPServerParams::Ptr http_params{new HTTPServerParams};    //!< Server parameters
        HTTPServer server;                                          //!< HTTP server

    public:
        /*!
        \brief Create HTTP server
        \param listen_to Server address
        */
        RestService(Logger& logger, const SocketAddress& listen_to)
            : server(new RestHandlerFactory(logger), ServerSocket(listen_to), http_params)
        {}

        /*!
        \brief Start HTTP server
        */
        void start()
        {
            server.start();
        }

        /*!
        \brief Stop HTTP server
        */
        void stop()
        {
            server.stop();
        }

        /*!
        \brief Stop HTTP server
        */
        ~RestService()
        {
            server.stop();
        }
    };

    //=========================
    // Main Poco Application
    //=========================

    /*!
    \brief Poco object for TPX3 raw stream analysis application
    */
    class Tpx3App final : public Application {

        constexpr static unsigned DEFAULT_BUFFER_SIZE = 1024;   //!< Default IO buffer size
        constexpr static unsigned DEFAULT_NUM_BUFFERS = 8;      //!< Default number of IO buffers
        // constexpr static unsigned DEFAULT_NUM_ANALYSERS = 6;

        Logger& logger;                 //!< Poco::Logger object
        int rval = Application::EXIT_OK;//!< Default application return value

        SocketAddress serverAddress = SocketAddress{"localhost:8080"};  //!< Default ASI server address
        SocketAddress clientAddress = SocketAddress{"127.0.0.1:8451"};  //!< Default raw data stream tcp destination (own address)
        SocketAddress controlAddress = SocketAddress{"127.0.0.1:8452"}; //!< Default control interface address (own address)

        std::unique_ptr<HTTPClientSession> clientSession;   //!< Client session with ASI server
        std::unique_ptr<ServerSocket> serverSocket;         //!< Socket for connecting to myself

        Poco::JSON::Parser jsonParser;  //!< Poco JSON parser

        std::string bpcFilePath;        //!< Path to ASI bpc detector configuration file (optional)
        std::string dacsFilePath;       //!< Path to ASI dacs detector configuration file (optional)
        std::string streamFilePath;     //!< Path (and flag) to file to which the raw event stream should be copied (don't copy if empty)

        int64_t initialPeriod;                          //!< Initial period interval in clock ticks
        double undisputedThreshold = 0.1;               //!< Default undisputed period interval threshold as ratio, [t..1-t] is undisputed
        unsigned long numBuffers = DEFAULT_NUM_BUFFERS; //!< Number of IO buffers
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE; //!< IO buffer size
        // unsigned long numAnalysers = DEFAULT_NUM_ANALYSERS;
        unsigned long numChips = 0;                     //!< Number of TPX3 chips on the detector
        unsigned long maxPeriodQueues = 4;              //!< Maximum number of remembered period interval changes

    protected:
        /*!
        \brief Poco application options definition
        \param options Poco options set
        */
        inline void defineOptions(OptionSet& options) override
        {
            Application::defineOptions(options);

            options.addOption(Option("loglevel", "l")
                .description("log level:\nfatal,critical,error,warning,\nnotice,information,debug,trace")
                .required(false)
                .repeatable(false)
                .argument("LEVEL")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleLogLevel)));

            options.addOption(Option("help", "h")
                .description("display help information")
                .required(false)
                .repeatable(false)
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleHelp)));

            options.addOption(Option("server", "s")
                .description("ASI server address")
                .required(false)
                .repeatable(false)
                .argument("ADDRESS")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleAddress)));

            options.addOption(Option("address", "a")
                .description("my address")
                .required(false)
                .repeatable(false)
                .argument("ADDRESS")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleAddress)));

            options.addOption(Option("control", "c")
                .description("control interface address")
                .required(false)
                .repeatable(false)
                .argument("ADDRESS")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleAddress)));

            options.addOption(Option("bpc-file", "b")
                .description("bpc file path")
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFilePath)));

            options.addOption(Option("dacs-file", "d")
                .description("dacs file path")
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFilePath)));

            options.addOption(Option("num-buffers", "n")
                .description("number of data buffers")
                .required(false)
                .repeatable(false)
                .argument("NUM")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));

            options.addOption(Option("buf-size", "N")
                .description("individual data buffer byte size,\nwill be rounded up to a multiple of 8")
                .required(false)
                .repeatable(false)
                .argument("NUM")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));

            options.addOption(Option("initial-period", "p")
                .description("initial TDC period")
                .required(true)
                .repeatable(false)
                .argument("NUM")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));

            options.addOption(Option("undisputed-threshold", "u")
                .description("undisputed part of period [T..1-T]")
                .required(false)
                .repeatable(false)
                .argument("T")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFloat)));

            options.addOption(Option("max-period-queues", "q")
                .description("maximum number of period reorder queues")
                .required(false)
                .repeatable(false)
                .argument("NUM")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));

            options.addOption(Option("stream-to-file", "f")
                .description("stream to file")
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFilePath)));

            options.addOption(Option("server-mode", "S")
                .description("run in server-mode")
                .required(false)
                .repeatable(false)
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleBool)));

            options.addOption(Option("version", "v")
                .description("show version")
                .required(false)
                .repeatable(false)
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleVersion)));
        }

        /*!
        \brief Log level option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleLogLevel(const std::string& name, const std::string& value)
        {
            logger.setLevel(Logger::parseLevel(value));
            logger << "handleLogLevel(" << name << ", " << value << ")" << log_trace;
        }

        /*!
        \brief Help option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleHelp(const std::string& name, const std::string& value)
        {
            logger << "handleHelp(" << name << ", " << value << ")" << log_trace;
            HelpFormatter helpFormatter(options());
            helpFormatter.setCommand(commandName());
            helpFormatter.setUsage("OPTIONS");
            helpFormatter.setHeader("Handle TimePix3 raw stream.");
            helpFormatter.format(std::cout);
            stopOptionsProcessing();
            global::instance->stop = true;
        }

        /*!
        \brief Boolean valued option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleBool(const std::string& name, const std::string& value)
        {
            logger << "handleNumber(" << name << ", " << value << ")" << log_trace;
            bool val = true;
            if ((value == "") ||
                (value == "true") ||
                (value == "1")) {
                ;
            } else if ((value == "false") ||
                        (value == "0")) {
                val = false;
            } else {
                throw InvalidArgumentException{std::string{"invalid value for argument: "} + name};
            }
            if (name == "server") {
                global::instance->server_mode = val;
            } else {
                throw LogicException{std::string{"unknown number argument name: "} + name};
            }
        }

        /*!
        \brief Integer valued option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleNumber(const std::string& name, const std::string& value)
        {
            logger << "handleNumber(" << name << ", " << value << ")" << log_trace;
            unsigned long num = 0;
            try {
                num = std::stoul(value);
            } catch (std::exception& ex) {
                throw InvalidArgumentException{std::string{"invalid value for argument: "} + name};
            }
            if (name == "buf-size") {
                if (num < 8)
                    throw InvalidArgumentException{"buffer size too small"};
                bufferSize = (num + 7ul) & ~7ul;
            } else if (name == "num-buffers") {
                if (num < 1)
                    throw InvalidArgumentException{"non-positive number of data buffers"};
                numBuffers = num;
            } else if (name == "initial-period") {
                if (num < 1)
                    throw InvalidArgumentException{"non-positive initial TDC period"};
                initialPeriod = num;
            } else if (name == "max-period-queues") {
                if (num < 1)
                    throw InvalidArgumentException{"non-positive maximum period queues"};
                maxPeriodQueues = num;
            } else {
                throw LogicException{std::string{"unknown number argument name: "} + name};
            }
        }

        /*!
        \brief Real valued option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleFloat(const std::string& name, const std::string& value)
        {
            logger << "handleFloat(" << name << ", " << value << ")" << log_trace;
            double val = .0;
            try {
                val = std::stod(value);
            } catch (std::exception& ex) {
                throw InvalidArgumentException{std::string{"invalid value for argument: "} + name};
            }
            if (name == "undisputed-threshold") {
                if ((val < .0) || (val > .5))
                    throw InvalidArgumentException{"undisputed-period outside of [0 .. 0.5]"};
                undisputedThreshold = val;
            } else {
                throw LogicException{std::string{"unknown float argument name: "} + name};
            }
        }

        /*!
        \brief IP address option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleAddress(const std::string& name, const std::string& value)
        {
            logger << "handleAddress(" << name << ", " << value << ')' << log_trace;
            if (name == "server") {
                try {
                    serverAddress = SocketAddress{value};
                } catch (Poco::Exception& ex) {
                    throw InvalidArgumentException{"server address", ex, __LINE__};
                }
            } else if (name == "address") {
                try {
                    clientAddress = SocketAddress{value};
                } catch (Poco::Exception& ex) {
                    throw InvalidArgumentException{"my address", ex, __LINE__};
                }
            } else if (name == "control") {
                try {
                    controlAddress = SocketAddress(value);
                } catch (Poco::Exception& ex) {
                    throw InvalidArgumentException{"control interface address", ex, __LINE__};
                }
            } else {
                throw LogicException{std::string{"unknown address argument name: "} + name};
            }
        }

        /*!
        \brief File path option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleFilePath(const std::string& name, const std::string& value)
        {
            logger << "handleFilePath(" << name << ", " << value << ')' << log_trace;
            if (name == "bpc-file")
                bpcFilePath = value;
            else if (name == "dacs-file")
                dacsFilePath = value;
            else if (name == "stream-to-file")
                streamFilePath = value;
            else
                throw LogicException{std::string{"unknown file path argument name: "} + name};
        }

        /*!
        \brief Version option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleVersion(const std::string& name, const std::string& value)
        {
            logger << "handleVersion(" << name << ", " << value << ')' << log_trace;
            std::cout << VERSION << '\n';
            stopOptionsProcessing();
            global::instance->stop = true;
        }

        /*!
        \brief Map request string to Uri
        \param requestString HTTP request string
        \return URI string
        */
        std::string getUri(const std::string& requestString)
        {
            logger << "getUri(" << requestString << ')' << log_trace;
            // std::ostringstream oss;
            // oss << "http:" << requestString;
            // return URI{oss.str()}.toString();
            return URI{requestString}.toString();
        }

        /*!
        \brief Check HTTP response
        \param response Poco HTTP response reference
        \param in       Poco HTTP response input stream reference
        \throw RuntimeException with error response if the response status is not OK
        */
        inline void checkResponse(const HTTPResponse& response, std::istream& in)
        {
            if (response.getStatus() != HTTPResponse::HTTP_OK) {
                std::ostringstream oss;
                oss << "request failed (" << response.getStatus() << "): " << response.getReason() << '\n' << in.rdbuf();
                throw RuntimeException(oss.str(), __LINE__);
            }
        }

        /*!
        \brief HTTP GET request to ASI server
        \param requestString    HTTP request string
        \param response         Poco HTTP response object reference
        \return Input stream reference for reading HTTP GET response content
        */
        inline std::istream& serverGet(const std::string& requestString, HTTPResponse& response)
        {
            logger << "serverGet(" << requestString << ')' << log_trace;
            auto request = HTTPRequest{HTTPRequest::HTTP_GET, getUri(requestString)};
            logger << request.getMethod() << " " << request.getURI() << log_debug;
            clientSession->sendRequest(request);
            return clientSession->receiveResponse(response);
        }

        /*!
        \brief HTTP PUT request to ASI server
        \param requestString    HTTP request string
        \param contentType      HTTP content type
        \param contentLength    HTTP content length
        \return Output stream object reference for content writing
        */
        inline std::ostream& serverPut(const std::string& requestString, const std::string& contentType, std::streamsize contentLength)
        {
            logger << "serverPut(" << requestString << ", " << contentType << ", " << contentLength << ')' << log_trace;
            auto request = HTTPRequest{HTTPRequest::HTTP_PUT, getUri(requestString)};
            request.setContentType(MediaType{contentType});
            request.setContentLength(contentLength);
            logger << request.getMethod() << " " << request.getURI() << log_debug;
            return clientSession->sendRequest(request);
        }

        /*!
        \brief Reset HTTP session if expected EOF is not seen
        \param in HTTP response input stream reference
        */
        inline void checkSession(std::istream& in)
        {
            logger << "checkSession(" << in.eof() << ")" << log_trace;
            if (in.eof())
                return;
            constexpr unsigned bufSize = 32;
            char buf[bufSize];
            in.read(buf, bufSize);
            if (! in.eof()) {
                logger << "session reset" << log_debug;
                clientSession.reset();
            }
        }

        /*!
        \brief HTTP GET request with JSON object response
        \param requestString HTTP request string
        \return Poco pointer to JSON object
        */
        inline Poco::JSON::Object::Ptr getJsonObject(const std::string& requestString)
        {
            logger << "getJsonObject(" << requestString << ")" << log_trace;
            jsonParser.reset();
            HTTPResponse response;
            auto& in = serverGet(requestString, response);
            checkResponse(response, in);
            Poco::JSON::Object::Ptr result = jsonParser.parse(in).extract<Poco::JSON::Object::Ptr>();
            checkSession(in);
            return result;
        }

        /*!
        \brief HTTP PUT request with JSON string argument
        \param requestString    HTTP request string
        \param jsonString       JSON object as a string
        \param response         Poco HTTP response object reference
        \return Input stream reference for reading response
        */
        inline std::istream& putJsonString(const std::string& requestString, const std::string& jsonString, HTTPResponse& response)
        {
            logger << "putJsonString(" << requestString << ", " << jsonString << ")" << log_trace;
            auto& out = serverPut(requestString, "application/json", jsonString.size());
            out << jsonString;
            return clientSession->receiveResponse(response);
        }

        /*!
        \brief HTTP PUT request with JSON object argument
        \param requestString    HTTP request string
        \param objPtr           Poco JSON object pointer
        \param response         Poco HTTP response object reference
        \return Input stream reference for reading response
        */
        inline std::istream& putJsonObject(const std::string& requestString, Poco::JSON::Object::Ptr objPtr, HTTPResponse& response)
        {
            logger << "putJsonObject(" << requestString << ")" << log_trace;
            std::ostringstream oss;
            objPtr->stringify(oss);
            return putJsonString(requestString, oss.str(), response);
        }

        /*!
        \brief Get ASI dashboard
        \return Poco pointer to ASI dashboard JSON object
        */
        inline Poco::JSON::Object::Ptr dashboard()
        {
            logger << "dashboard()" << log_trace;
            return getJsonObject("/dashboard");
        }

        /*!
        \brief Detector initialization request to ASI server
        */
        inline void detectorInit()
        {
            logger << "detectorInit()" << log_trace;
            HTTPResponse response;
            if (! bpcFilePath.empty()) {
                auto& in = serverGet(std::string("/config/load?format=pixelconfig&file=") + bpcFilePath, response);
                checkResponse(response, in);
                logger << "Response of loading binary pixel configuration file: " << in.rdbuf() << log_notice;
                checkSession(in);
            }
            if (! dacsFilePath.empty()) {
                auto& in = serverGet(std::string("/config/load?format=dacs&file=") + dacsFilePath, response);
                checkResponse(response, in);
                logger << "Response of loading dacs file: " << in.rdbuf() << log_notice;
                checkSession(in);
            }
        }

        /*!
        \brief Get detector configuration JSON object from ASI server
        \return Poco pointer to detector configuration JSON object
        */
        inline Poco::JSON::Object::Ptr detectorConfig()
        {
            logger << "detectorConfig()" << log_trace;
            return getJsonObject("/detector/config");
        }

        /*!
        \brief Get detector info JSON object from ASI server
        \return Poco pointer to detector info JSON object
        */
        inline Poco::JSON::Object::Ptr detectorInfo()
        {
            logger << "detectorInfo()" << log_trace;
            return getJsonObject("/detector/info");
        }

        /*!
        \brief Get detector layout JSON object from ASI server
        \return Poco pointer to detector layout JSON object
        */
        inline Poco::JSON::Object::Ptr detectorLayout()
        {
            logger << "detectorLayout()" << log_trace;
            return getJsonObject("/detector/layout");
        }

        // void acquisitionInit(Poco::JSON::Object::Ptr configPtr, unsigned numTriggers, unsigned shutter_open_ms=490u, unsigned shutter_closed_ms=10u)
        // {
        //     logger << "acquisitionInit(" << numTriggers << ", " << shutter_open_ms << ", " << shutter_closed_ms << ")" << log_trace;
        //     configPtr->set("nTriggers", numTriggers);
        //     configPtr->set("TriggerMode", "AUTOTRIGSTART_TIMERSTOP");
        //     configPtr->set("TriggerPeriod", (shutter_open_ms + shutter_closed_ms) / 1000.f);
        //     configPtr->set("ExposureTime", shutter_open_ms / 1000.f);

        //     HTTPResponse response;
        //     auto& in = putJsonObject("/detector/config", configPtr, response);
        //     checkResponse(response, in);
        //     logger << "Response of loading binary pixel configuration file: " << in.rdbuf() << log_notice;
        //     checkSession(in);
        // }

        /*!
        \brief Send raw event stream destination IP and port information to ASI server
        \param address TCP address
        */
        void serverRawDestination(const SocketAddress& address)
        {
            logger << "serverRawDestination(" << address.toString() << ")" << log_trace;
            HTTPResponse response;
            std::string destinationJsonString = R"({ "Raw": [{ "Base": "tcp://connect@)" + address.toString() + R"(" }] })";
            auto& in = putJsonString("/server/destination", destinationJsonString, response);
            checkResponse(response, in);
            logger << "Response of uploading the Destination Configuration to SERVAL : " << in.rdbuf() << log_notice;
            checkSession(in);
        }

        /*!
        \brief Send aquisition start signal to ASI server
        */
        void acquisitionStart()
        {
            logger << "acquisitionStart()" << log_trace;
            HTTPResponse response;
            auto& in = serverGet("/measurement/start", response);
            checkResponse(response, in);
            logger << "Response of acquisition start: " << in.rdbuf() << log_notice;
            checkSession(in);
        }

        /*!
        \brief Poco application main function
        \param args Positional commandline args
        \return 0 for ok
        */
        inline int main(const std::vector<std::string>& args) override
        {
            {
                auto log_proxy = logger << "main(";
                for (const auto& arg : args)
                    log_proxy << ' ' << arg;
                log_proxy << " )" << log_trace;
            }

            logger << "running on process " << Poco::Process::id() << log_info;

            if (global::instance->stop)
                return rval;

            global::instance->get_callbacks["/?stop"] = [](const std::string& val) -> std::string {
                if (val == "true") {
                    global::instance->stop = true;
                    return "OK";
                }
                throw Poco::DataFormatException("only 'true' is accepted as 'stop' value");
            };

            global::instance->get_callbacks["/?start"] = [](const std::string& val) -> std::string {
                if (val == "true") {
                    global::instance->start = true;
                    return "OK";
                }
                throw Poco::DataFormatException("only 'true' is accepted as 'start' value");
            };

            // ---------------- BEGIN test code -------
            // TODO: remove
            global::instance->put_callbacks["/echo"] = [](Poco::JSON::Object::Ptr obj) -> std::string {
                std::ostringstream oss;
                obj->stringify(oss);
                return oss.str();
            };

            RestService restService(logger, controlAddress);
            logger << "listen for commands on " << controlAddress.toString() << log_info;
            restService.start();

            {
                using namespace std::chrono_literals;
                while (!global::instance->stop) {
                    std::cout << "doing something...\n";
                    std::this_thread::sleep_for(1s);
                }
                restService.stop();
                std::exit(0);
            }
            // ---------------- END test code ---------

            logger << "connecting to ASI server at " << serverAddress.toString() << log_notice;
            clientSession.reset(new HTTPClientSession{serverAddress});

            auto dashboardPtr = dashboard();
            std::string softwareVersion = (dashboardPtr|"Server")->getValue<std::string>("SoftwareVersion");
            {
                LogProxy log(logger);
                log << "Server Software Version: " << softwareVersion << "\nDashboard: ";
                dashboardPtr->stringify(log.base());
                log << log_notice;
            }

            detectorInit();

            {
                auto configPtr = detectorConfig();
                {
                    LogProxy log(logger);
                    log << "Response of getting the Detector Configuration from SERVAL: ";
                    configPtr->stringify(log.base());
                    log << log_notice;
                }

                // unsigned numTriggers = 1;
                // acquisitionInit(configPtr, numTriggers, 2, 950);
            }

            {
                auto infoPtr = detectorInfo();
                {
                    LogProxy log(logger);
                    log << "Response of getting the Detector Info from SERVAL: ";
                    infoPtr->stringify(log.base());
                    log << log_notice;
                }

                infoPtr->get("NumberOfChips").convert(numChips);
            }

            detector_layout layout;
            {
                auto layoutPtr = detectorLayout();
                {
                    LogProxy log(logger);
                    log << "Response of getting the Detector Layout from SERVAL: ";
                    layoutPtr->stringify(log.base());
                    log << log_notice;
                }

                auto origPtr = layoutPtr | "Original";
                origPtr->get("Width").convert(layout.width);
                origPtr->get("Height").convert(layout.height);

                auto chipPtr = origPtr / "Chips";
                for (decltype(numChips) i=0; i<numChips; i++) {
                    chip_position chip;
                    (chipPtr | i)->get("X").convert(chip.x);
                    (chipPtr | i)->get("Y").convert(chip.y);
                    layout.chip.push_back(chip);
                }

                {
                    LogProxy log(logger);
                    log << "layout: " << layout.width << ',' << layout.height << ':';
                    for (decltype(numChips) i=0; i<numChips; i++)
                        log << ' ' << layout.chip[i].x << ',' << layout.chip[i].y;
                    log << log_debug;
                }
            }

            processing::init(layout);

            logger << "listening at " << clientAddress.toString() << log_notice;
            serverSocket.reset(new ServerSocket{clientAddress});

            serverRawDestination(clientAddress);

            acquisitionStart();

            SocketAddress senderAddress;
            StreamSocket dataStream = serverSocket->acceptConnection(senderAddress);

            if (! streamFilePath.empty()) {
                const auto t1 = wall_clock::now();

                CopyHandler copyHandler(dataStream, streamFilePath, logger);
                copyHandler.run_async();
                copyHandler.await();

                const auto t2 = wall_clock::now();
                const double time = std::chrono::duration<double>{t2 - t1}.count();

                logger << "time: " << time << "s\n";
            } else {
                const auto t1 = wall_clock::now();

                logger << "connection from " << senderAddress.toString() << log_info;

                DataHandler<AsiRawStreamDecoder> dataHandler(dataStream, logger, bufferSize, numChips, initialPeriod, undisputedThreshold, maxPeriodQueues);
                dataHandler.run_async();
                dataHandler.await();

                const auto t2 = wall_clock::now();
                const double time = std::chrono::duration<double>{t2 - t1}.count();

                dataStream.close();

                const uint64_t hits = dataHandler.hitCount;
                logger << "time: " << time << "s, hits: " << hits << ", rate: " << (hits / time) << " hits/s\n"
                    << "analysis spin: " << dataHandler.analyseSpinTime << "s, work: " << dataHandler.analyseTime
                    << "\nreading spin: " << dataHandler.readSpinTime << "s, work: " << dataHandler.readTime << log_notice;
            }

            return Application::EXIT_OK;
        }

    public:
        /*!
        \brief Constructor
        \param log  Poco::Logger object
        \param argc Number of commandline arguments
        \param argv Commandline argument values
        */
        explicit Tpx3App(Logger& log, int argc, char* argv[])
            : logger(log)
        {
            init(argc, argv);
        }

        inline virtual ~Tpx3App() {}
    };

} // namespace

/*!
\brief Entry function
\param argc Number of commandline parameters
\param argv Values of commandliine parameters
\return 0 if no error, not 0 otherwise
*/
int main (int argc, char* argv[])
{
    try {
        Logger& logger = Logger::get("Tpx3App");

        try {
            logger.setLevel(Message::PRIO_CRITICAL);
            Tpx3App app(logger, argc, argv);
            return app.run();
        } catch (Poco::Exception& ex) {
            LogProxy log(logger);
            log << ex.displayText() << '\n';
            const Poco::Exception* pEx = & ex;
            while ((pEx = pEx->nested())) {
                log << "  " << pEx->displayText() << '\n';
            }
            log << log_fatal;
        } catch (std::exception& ex) {
            logger << "Exception: " << ex.what() << log_fatal;
        }

    } catch (Poco::Exception& ex) {
        std::cerr << "fatal error: " << ex.displayText() << log_fatal;
    } catch (std::exception& ex) {
        std::cerr << "fatal error: " << ex.what() << '\n';
    }
}

/*!
\mainpage ASI TPX3 Detector Event Analysis Software

\section intro_sec Introduction

The software consists of

- tpx3app\n
    Analysis program generating histogram output by receiving raw event data through a TCP stream from the ASI server
- test\n
    Unit tests for some of the tpx3app components
- server\n
    ASI server raw event stream replay server

\section design_sec Design

Currently, in analysis mode, the process is devided into

- a main thread (see main.cpp) that interacts with the ASI server
- a reader thread (see data_handler.h) that reads the TCP raw event data stream produced by the ASI server
- per chip analysis threads (see data_handler.h) that dispatch events from the raw stream, build and write the histograms

The incoming data stream is split into per chip data streams.

\image html io_buffers.png width=80%

The raw event stream from the ASI server comes in packets per chip.
These packets are distributed by a single reader thread to per chip reordering IO buffer queues (see io_buffers.h).
Per chip there's a single analysis thread that dispatches events from IO buffers.

Every analysis thread deals with event data originating from a single detector chip.

\image html period_queues.png width=80%

The period changes for which no TDC has been seen yet, are predicted (see period_predictor.h).
Events that fall into a disputed interval (see undisputedThreshold) around expected period changes are put into a reordering queue (see event_reordering.h).
Such reordering queues are maintained for a number of recent period changes (see maxPeriodQueues and period_queues.h). Events for which period number assignment
is undisputed - because they don't fall into a disputed interval, or because the TDC of the disputed interval has been seen - are sent to the histogramming code
(see processing.cpp). The histogram is saved and cleared periodically (see save_interval).

\section issues_sec Issues

- The parallelization into and synchronization between threads is probably too simple to be fast.
- Logging and error handling are implemented for debugging right now, which might be too slow.
- Missing requirements for many aspects, like logging, exception handling, and configuration.
- Missing requirements for the software environment the analysis process will be embedded into.
- No functional verification has been done yet.

\section unit_test Unit Tests

To run the unit tests, assuming your C++ compiler is g++-11:

\code{.unparsed}
$ CXX=g++-11 ./compile.sh test
$ ./test
\endcode

This should give you a list of executed tests with all OK as test result. Extra arguments are documented through the --help option.
The test executable takes a C++ regular expression as filter pattern, for example.

\section example_run Example Run

In order to get some test output, the tpx3app and server executables have to be compiled. Assuming your C++ compiler is g++-11:

\code{.unparsed}
$ CXX=g++-11 ./compile.sh
$ CXX=g++-11 ./compile.sh server
\endcode

Then the mock ASI server has to be started with a captured raw event stream file and the number of TPX3 chips used to produce the stream,
as an example:

\code{.unparsed}
$ ./server  --input=/data/TCP-raw/event-stream-v3-asynch-70Kcs.raw --nchips=4
\endcode

The server will print TCP address information for the TCP address it is listening on. The --help option documents other options.

Now the analysis application can be started. For this the tpx3app needs a number of inputs, the specification of which is not
very consistent, unfortunately, due to lack of specifications.

- "XESPoints.inp" is a file with hardcoded name in the current directory (see readAreaROI() function in processing.cpp) specifying
  the mapping between detector pixels and energy points.
- "Processing.ini" is a file with hardcoded name in the current directory (see init() function in processing.cpp) specifying
  time ROI and output files.
- Commandline options documented through the --help option. All of them have defaults which should make sense for well behaved data
  and TCP adresses. The --max-period-queues option gives the size of the period changes memory described above.

Assuming you have input fils that make sense:

\code{.unparsed}
$ ./tpx3app --initial-period=5000 --max-period-queues=6 --loglevel=warning
\endcode

And hopefully you'll have some output in the folder specified via the Processing.inp file.
*/
