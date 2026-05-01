#pragma once

#ifndef REST_CALLBACKS_H
#define REST_CALLBACKS_H

/*!
\file
Provide REST functionality
*/

#include <mutex>
#include <fstream>

#include <Poco/URI.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include "Poco/JSON/Parser.h"
#include <Poco/JSON/PrintHandler.h>
#include <Poco/Net/WebSocket.h>

#include "logging.h"
#include "global.h"
#include "version.h"

namespace {
    using Poco::URI;
    using Poco::Net::SocketAddress;
    using Poco::Net::ServerSocket;
    using Poco::Net::HTTPResponse;
    using Poco::Net::HTTPRequestHandlerFactory;
    using Poco::Net::HTTPServerParams;
    using Poco::Net::HTTPServer;
    using Poco::Net::HTTPRequestHandler;
    using Poco::Net::HTTPServerRequest;
    using Poco::Net::HTTPServerResponse;
    using Poco::JSON::PrintHandler;
    using Poco::Net::WebSocket;

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
        RestHandler(Logger& logger_) noexcept
            : logger(logger_)
        {}

        /*!
        \brief Handle rest requests
        \param request Rest request
        \param response Rest response
        */
        void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override {
            std::string response_text;
            try {
                URI uri{request.getURI()};
                logger << request.getMethod() << " Request: " << uri.toString() << log_notice;
                std::string key = uri.getPath();
                if (request.getMethod() == Poco::Net::HTTPRequest::HTTP_PUT) {
                    if (request.getContentType() != "application/json")
                        throw Poco::DataFormatException{"PUT only allowed with JSON content"};
                    const auto& callbacks = global::instance->put_callbacks;
                    try {
                        const auto& handle = callbacks.at(key);
                        if (handle.index() == 0) {
                            jsonParser.reset();
                            Poco::JSON::Object::Ptr result = jsonParser
                                .parse(request.stream())
                                .extract<Poco::JSON::Object::Ptr>();
                            response_text = std::get<0>(handle)(result);
                        } else if (handle.index() == 1) {
                            response_text = std::get<1>(handle)(request.stream());
                        } else
                            throw Poco::LogicException{"Unknown put handler variant"};
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
                        throw Poco::DataFormatException("Only one key is allowed per request");
                    }
                    const auto& callbacks = global::instance->get_callbacks;
                    try {
                        response_text = callbacks.at(key)(val);
                    } catch (std::out_of_range&) {
                        throw Poco::DataFormatException(std::string("illegal path/key - ") + key);
                    }
                } else {
                    throw Poco::DataFormatException(std::string("Unsupported method: ") + request.getMethod());
                }
            } catch (Poco::Exception& ex) {
                response_text = ex.displayText();
                response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST, ex.displayText());
            } catch (const std::exception& ex) {
                response_text = ex.what();
                response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST, ex.what());
            }

            try {
                response.send() << response_text;
                logger << "Response status: " << response.getStatus() << ", Reson: " << response.getReason() << log_debug;
            } catch (std::exception& ex) {
                logger << "Unable to handle REST call - " << ex.what() << log_error;
            }
        }
    };

    /*!
    \brief Handle WebsSocket for the program state
    \see global
    */
    class StateHandler final : public HTTPRequestHandler
    {
        Logger& logger;                             //!< Logger
        static inline std::unique_ptr<WebSocket> ws;//!< single WebSocket
        static inline std::mutex ws_mutex;          //!< Protect WebSocket
        static inline std::atomic_bool stop_sig;    //!< Stop signal

    public:
        /*!
        \brief Constructor
        \param _logger Logging proxy
        */
        explicit StateHandler(Logger& _logger) noexcept
            : logger(_logger)
        {}

        /*!
        \brief Request handler
        \param request Poco server request
        \param response Pocoe server response
        */
        void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
        {
            try {
                {
                    std::lock_guard lock(ws_mutex);
                    ws.reset(new WebSocket(request, response));
                    logger << "websocket: created" << log_debug;
                    std::string_view state{global::instance->state};
                    ws->setReceiveTimeout(Poco::Timespan(1,0));
                    ws->sendFrame(state.data(), state.size(), WebSocket::FRAME_TEXT);
                }

                static constexpr int buf_sz = 1024;
                char buffer[buf_sz];
                int flags, n;

                while ((ws != nullptr) && !stop_sig) {
                    try {
                        n = ws->receiveFrame(buffer, sizeof(buffer), flags);
                    } catch (Poco::TimeoutException&) {
                        continue;
                    }
                    logger << "websocket: frame n=" << n << ", flags=" << flags << log_debug;

                    if (flags & WebSocket::FRAME_OP_PING) {
                        // Respond to PING with PONG
                        ws->sendFrame(buffer, n, WebSocket::FRAME_FLAG_FIN | WebSocket::FRAME_OP_PONG);
                        logger << "websocket: ping->pong" << log_debug;
                    } else if (n == 0 || (flags & WebSocket::FRAME_OP_CLOSE)) {
                        logger << "websocket: closed" << log_debug;
                        break; // client closed connection
                    } else if ((n > 0) && (n < buf_sz)) { // echo message for tests
                        ws->sendFrame(buffer, n, WebSocket::FRAME_TEXT);
                        buffer[n] = 0;
                        logger << "websocket: echo \"" << buffer << '"' << log_info;
                    }
                }

            } catch (std::exception& exc) {
                logger << "websocket: error - " << exc.what() << log_warn;
            }

            {
                std::lock_guard<std::mutex> lock(ws_mutex);
                if (ws != nullptr) {
                    ws->shutdown();
                    ws.reset(nullptr);
                }
            }

            logger << "websocket: gone" << log_debug;
        }

        /*!
        \brief Set program state
        \param state New program state
        */
        static void set_state(const std::string_view& state)
        {
            if (global::instance->server_mode) {
                std::lock_guard<std::mutex> lock(ws_mutex);
                global::instance->state = state;
                if (ws == nullptr)
                    return;
                ws->sendFrame(state.data(), state.size(), WebSocket::FRAME_TEXT);
            } else {
                global::instance->state = state;
            }
        }

        /*!
        \brief Stop websocket
        */
        static void stop() noexcept
        {
            stop_sig = true;
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
        RestHandlerFactory(Logger& logger_) noexcept
            : logger(logger_)
        {}

        /*!
        \brief Create rest handler
        \param request Rest request
        \return Handler for rest requests
        */
        HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override {
            if (request.getURI() == "/ws")
                return new StateHandler(logger);
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
        \param logger Logger proxy
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
} // anonymous namespace

/*!
\brief Functions for initializing, starting, and stopping the REST service
*/
namespace rest {

    /*!
    \brief Initialize REST callbacks
    */
    inline void init_callbacks()
    {
        // ----------------------- setup and start rest service -----------------------
        // REST COMMAND
        // /?stop=true  GET to a stop now
        // returns:
        // - status 200
        // - data OK
        // when: after init
        global::instance->get_callbacks["/?stop"] = [](const std::string& val) -> std::string {
            auto& gvars = *global::instance;
            if (val == "true") {
                gvars.stop.store(true);
                for (const auto& handler : gvars.stop_handlers)
                    handler();
                return "OK";
            }
            throw Poco::DataFormatException("only 'true' is accepted as 'stop' value");
        };

        // REST COMMAND
        // /?restart=true  GET to a restart now
        // returns:
        // - status 200
        // - data OK
        // when: after init
        global::instance->get_callbacks["/?restart"] = [](const std::string& val) -> std::string {
            auto& gvars = *global::instance;
            if (val == "true") {
                gvars.restart.store(true);
                gvars.stop.store(true);
                for (const auto& handler : gvars.stop_handlers)
                    handler();
                return "OK";
            }
            throw Poco::DataFormatException("only 'true' is accepted as 'stop' value");
        };

        // REST COMMAND
        // /?stop_collect=true  GET stop collecting data
        // returns:
        // - status 200
        // - data OK
        // when: await_connection and collect
        global::instance->get_callbacks["/?stop_collect"] = [](const std::string& val) -> std::string {
            auto& gvars = *global::instance;
            if (val == "true") {
                gvars.stop_collect.store(true);
                for (const auto& handler : gvars.stop_handlers)
                    handler();
                return "OK";
            }
            throw Poco::DataFormatException("only 'true' is accepted as 'stop_collect' value");
        };

        // REST COMMAND
        // /?kill=true  GET process killed
        // no return
        // when: after init
        global::instance->get_callbacks["/?kill"] = [](const std::string& val) -> std::string {
            if (val == "true") {
                std::exit(EXIT_FAILURE);
                throw Poco::LogicException("should be unreachable");
            }
            throw Poco::DataFormatException("only 'true' is accepted as 'kill' value");
        };

        // REST COMMAND
        // /last-error  GET and reset last error message
        // return:
        // - status 200
        // - data {"type":"LastError","message":"none"}
        // when: after init
        global::instance->get_callbacks["/last-error"] = []([[maybe_unused]] const std::string& val) -> std::string {
            std::ostringstream oss;
            {
                Poco::JSON::PrintHandler json{oss};
                std::string err;
                std::swap(err, global::instance->last_error);
                json.startObject();
                json.key("type"); json.value(std::string{"LastError"});
                json.key("message"); json.value(err.empty() ? std::string{global::no_error} : err);
                json.endObject();
            }
            return oss.str();
        };

        // REST COMMAND
        // /state GET program state
        // return:
        // - status 200
        // - data {"type":"ProgramState","state":"config"} see global.h
        // when: after init
        global::instance->get_callbacks["/state"] = []([[maybe_unused]] const std::string& val) -> std::string {
            std::ostringstream oss;
            oss << R"({"type":"ProgramState","state":")" << global::instance->state << R"("})";
            return oss.str();
        };

        // REST COMMAND
        // /version  GET version string
        // return:
        // - status 200
        // - data {"type":"VersionString","version":"dev 9adfe29 2025-05-23"}
        // when: after init
        global::instance->get_callbacks["/version"] = []([[maybe_unused]] const std::string& val) -> std::string {
            std::ostringstream oss;
            oss << R"({"type":"VersionString","version":")" << VERSION << R"("})";
            return oss.str();
        };

        if (global::instance->server_mode) {
            // REST COMMAND
            // /?start=true  GET started preparing for data taking
            // return:
            // - status 200
            // - data OK
            // when: config in server-mode
            global::instance->get_callbacks["/?start"] = [](const std::string& val) -> std::string {
                if (val == "true") {
                    auto& gvars = *global::instance;
                    if (gvars.state != "config")
                        throw Poco::RuntimeException("not in config state");
                    gvars.start = true;
                    return "OK";
                }
                throw Poco::DataFormatException("only 'true' is accepted as 'start' value");
            };

            // REST COMMAND
            // /pixel-map  GET and PUT pixel mapping to energy points, see PixelIndexToEp::from_json
            // GET return:
            // - status 200
            // - data see energy_points.cpp from_json()
            // {
            //  "type": "PixelMap",        // optional
            //  "chips": [                 // per chip mapping
            //   [                         // chip 0: pixel mapping (256x256 pixels)
            //    {                        // chip 0, flat pixel 0: mapping
            //     "i":0,                  // flat pixel index (x*256+y)
            //     "p":[0,1,2],            // energy points
            //     "f":[0.33,0.33,0.33]    // energy fractions
            //    },
            //    ...                      // chip 0: other pixels
            //   ],
            //   ...                       // other chips
            //  ]
            // }
            // when: config in server-mode
            constexpr const char* rest_pmap = "/pixel-map";
            global::instance->get_callbacks[rest_pmap] = []([[maybe_unused]] const std::string& val) -> std::string {
                std::ostringstream oss;
                const auto& pmap_p = global::instance->pix_map;
                if (pmap_p == nullptr)
                    throw Poco::RuntimeException("pixel map has not been set");
                oss << *pmap_p;
                return oss.str();
            };


            // use description above for command extraction
            // /pixel-map  GET and PUT pixel mapping to energy points, see PixelIndexToEp::from_json
            // PUT return:
            // - status 200
            // - data OK
            // when: config in server-mode
            global::instance->put_callbacks[rest_pmap] = [](std::istream& in) -> std::string {
                auto& gvars = *global::instance;
                if (gvars.state != "config")
                    throw Poco::RuntimeException("not in config state");
                std::unique_ptr<PixelIndexToEp> pmap{new PixelIndexToEp};
                PixelIndexToEp::from(*pmap, in, PixelIndexToEp::JSON_STREAM);
                gvars.pix_map = pmap->to_map();
                return "OK";
            };

            // REST COMMAND
            // /pixel-map-from-file  PUT pixel mapping to energy points, see PixelIndexToEp::from_file
            // returns:
            // - status 200
            // - data OK
            // {
            //  "type": "PixelMapFromFile",
            //  "file": "path/to/file"
            // }
            // when: config in server-mode
            global::instance->put_callbacks["/pixel-map-from-file"] = [](Poco::JSON::Object::Ptr obj) -> std::string {
                auto& gvars = *global::instance;
                if (gvars.state != "config")
                    throw Poco::RuntimeException("not in config state");
                std::ifstream ifs{obj->getValue<std::string>("file")};
                std::unique_ptr<PixelIndexToEp> pmap{new PixelIndexToEp};
                PixelIndexToEp::from(*pmap, ifs);
                gvars.pix_map = pmap->to_map();
                return "OK";
            };

            // REST COMMAND
            // /other-config  GET and PUT other config, see global.h
            // GET return:
            // - status 200
            // - data
            // {
            //  "type": "OtherConfig",
            //  "output_uri": "tcp:localhost:3015", or "file:./dump"
            //  "save_interval": 131000,
            //  "TRoiStart": 0,
            //  "TRoiStep: 1,
            //  "TRoiN": 5000
            // }
            // when: config in server-mode
            constexpr const char* rest_config = "/other-config";
            global::instance->get_callbacks[rest_config] = []([[maybe_unused]] const std::string& val) -> std::string {
                std::ostringstream oss;
                const auto& gvars = *global::instance;
                oss << R"({"type":"OtherConfig","output_uri":")" << gvars.output_uri << '"'
                    << R"(,"save_interval":)" << gvars.save_interval
                    << R"(,"TRoiStart":)" << gvars.TRoiStart
                    << R"(,"TRoiStep":)" << gvars.TRoiStep
                    << R"(,"TRoiN":)" << gvars.TRoiN
                    << '}';
                return oss.str();
            };

            // use description above for command extraction
            // /other-config  GET and PUT other config, see global
            // PUT return:
            // - status 200
            // - data OK
            // when: config in server-mode
            global::instance->put_callbacks[rest_config] = [](Poco::JSON::Object::Ptr obj) -> std::string {
                auto& gvars = *global::instance;
                if (gvars.state != "config")
                    throw Poco::RuntimeException("not in config state");
                gvars.output_uri = obj->getValue<decltype(gvars.output_uri)>("output_uri");
                gvars.save_interval = obj->getValue<decltype(gvars.save_interval)::value_type>("save_interval");
                gvars.TRoiStart = obj->getValue<decltype(gvars.TRoiStart)::value_type>("TRoiStart");
                gvars.TRoiStep = obj->getValue<decltype(gvars.TRoiStep)::value_type>("TRoiStep");
                gvars.TRoiN = obj->getValue<decltype(gvars.TRoiN)::value_type>("TRoiN");
                return "OK";
            };

            // REST COMMAND
            // /net-addresses  GET applicable net addresses
            // GET return:
            // - status 200
            // - data
            // {
            //  "type":"NetAddresses",
            //  "control":"127.0.0.1:8452",     // own rest interface
            //  "address":"127.0.0.1:8451",     // own address, the destination of ASI server raw data
            //  "server":"127.0.0.1:8080"       // ASI server rest interface address
            // }
            // when: after init in server-mode
            global::instance->get_callbacks["/net-addresses"] = []([[maybe_unused]] const std::string& val) -> std::string {
                auto& gvars = *global::instance;
                std::ostringstream oss;
                oss << R"({"type":"NetAddresses","control":")" << gvars.controlAddress.toString() << '"'
                    << R"(,"address":")" << gvars.clientAddress.toString() << '"'
                    << R"(,"server":")" << gvars.serverAddress.toString() << R"("})";
                return oss.str();
            };
        } // if server-mode

        // REST COMMAND
        // /echo  PUT json data that is echoed (for testing)
        // return:
        // - status 200
        // - data same as input (in json format)
        // when: after init
        global::instance->put_callbacks["/echo"] = [](Poco::JSON::Object::Ptr obj) -> std::string {
            std::ostringstream oss;
            obj->stringify(oss);
            return oss.str();
        };
    }

    /*!
    \brief Start REST service
    \param logger Logger object
    \param controlAddress On this address
    \return REST service smart pointer
    */
    [[nodiscard("Rest service will not be started")]] inline std::unique_ptr<RestService> start_service(Logger& logger, const SocketAddress& controlAddress)
    {
        std::unique_ptr<RestService> restService{new RestService{logger, controlAddress}};
        logger << "running in " << (global::instance->server_mode ? "server" : "application") << " mode, listen for commands on " << controlAddress.toString() << log_notice;
        restService->start();
        return restService;
    }

    /*!
    \brief Stop REST service
    \param restService REST service pointer
    */
    inline void stop_service(RestService* restService)
    {
        if (restService)
            restService->stop();
    }

} // namespace rest

#endif // REST_CALLBACKS_H
