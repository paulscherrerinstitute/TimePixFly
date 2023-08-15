// Implement raw-stream-.example.py in C++

// Author: hans-christian.stadler@psi.ch

// #include <filesystem>
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
// #include <string_view>
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
#include "Poco/Net/MediaType.h"
#include "Poco/URI.h"
#include "Poco/Process.h"
#include "Poco/Exception.h"

#include "logging.h"
#include "decoder.h"
#include "data_handler.h"
#include "processing.h"

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
    using Poco::Net::MediaType;
    using Poco::URI;
    using Poco::LogicException;
    using Poco::InvalidArgumentException;
    using Poco::RuntimeException;
    using Poco::Dynamic::Var;
    using wall_clock = std::chrono::high_resolution_clock;

    inline Poco::JSON::Object::Ptr operator|(Poco::JSON::Object::Ptr object, const std::string& name)
    {
        Poco::JSON::Object::Ptr objPtr = object->getObject(name);
        if (objPtr.isNull())
            throw RuntimeException(std::string("JSON Object contains no object ") + name, __LINE__);
        return objPtr;
    }

    // inline Poco::JSON::Array::Ptr operator||(Poco::JSON::Object::Ptr object, const std::string& name)
    // {
    //     Poco::JSON::Array::Ptr arrayPtr = object->getArray(name);
    //     if (arrayPtr.isNull())
    //         throw RuntimeException(std::string("JSON Object contains no array ") + name, __LINE__);
    //     return arrayPtr;
    // }

    class Tpx3App final : public Application {

        constexpr static unsigned DEFAULT_BUFFER_SIZE = 1024;
        constexpr static unsigned DEFAULT_NUM_BUFFERS = 8;
        // constexpr static unsigned DEFAULT_NUM_ANALYSERS = 6;

        Logger& logger;
        bool stop = false;
        int rval = Application::EXIT_OK;

        SocketAddress serverAddress = SocketAddress{"localhost:8080"};  // ASI server address
        SocketAddress clientAddress = SocketAddress{"127.0.0.1:8451"};  // myself (raw data tcp destination)

        std::unique_ptr<HTTPClientSession> clientSession;   // client session with ASI server
        std::unique_ptr<ServerSocket> serverSocket;         // for connecting to myself

        Poco::JSON::Parser jsonParser;

        std::string bpcFilePath;
        std::string dacsFilePath;

        int64_t initialPeriod;
        double undisputedThreshold = 0.1;
        unsigned long numBuffers = DEFAULT_NUM_BUFFERS;
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE;
        // unsigned long numAnalysers = DEFAULT_NUM_ANALYSERS;
        unsigned long numChips = 0;
        unsigned long maxPeriodQueues = 4;

    protected:
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

            options.addOption(Option("bpc-file", "b")
                .description("bpc file path")
                .required(true)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFilePath)));

            options.addOption(Option("dacs-file", "d")
                .description("dacs file path")
                .required(true)
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
        }

        inline void handleLogLevel(const std::string& name, const std::string& value)
        {
            logger.setLevel(Logger::parseLevel(value));
            logger << "handleLogLevel(" << name << ", " << value << ")" << log_trace;
        }

        inline void handleHelp(const std::string& name, const std::string& value)
        {
            logger << "handleHelp(" << name << ", " << value << ")" << log_trace;
            HelpFormatter helpFormatter(options());
            helpFormatter.setCommand(commandName());
            helpFormatter.setUsage("OPTIONS");
            helpFormatter.setHeader("Handle TimePix3 raw stream.");
            helpFormatter.format(std::cout);
            stopOptionsProcessing();
            stop = true;
        }

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
            } else {
                throw LogicException{std::string{"unknown address argument name: "} + name};
            }
        }

        inline void handleFilePath(const std::string& name, const std::string& value)
        {
            logger << "handleFilePath(" << name << ", " << value << ')' << log_trace;
            if (name == "bpc-file")
                bpcFilePath = value;
            else if (name == "dacs-file")
                dacsFilePath = value;
            else
                throw LogicException{std::string{"unknown file path argument name: "} + name};
        }

        std::string getUri(const std::string& requestString)
        {
            logger << "getUri(" << requestString << ')' << log_trace;
            // std::ostringstream oss;
            // oss << "http:" << requestString;
            // return URI{oss.str()}.toString();
            return URI{requestString}.toString();
        }

        inline void checkResponse(const HTTPResponse& response, std::istream& in)
        {
            if (response.getStatus() != HTTPResponse::HTTP_OK) {
                std::ostringstream oss;
                oss << "request failed (" << response.getStatus() << "): " << response.getReason() << '\n' << in.rdbuf();
                throw RuntimeException(oss.str(), __LINE__);
            }
        }

        inline std::istream& serverGet(const std::string& requestString, HTTPResponse& response)
        {
            logger << "serverGet(" << requestString << ')' << log_trace;
            auto request = HTTPRequest{HTTPRequest::HTTP_GET, getUri(requestString)};
            logger << request.getMethod() << " " << request.getURI() << log_debug;
            clientSession->sendRequest(request);
            return clientSession->receiveResponse(response);
        }

        inline std::ostream& serverPut(const std::string& requestString, const std::string& contentType, std::streamsize contentLength)
        {
            logger << "serverPut(" << requestString << ", " << contentType << ", " << contentLength << ')' << log_trace;
            auto request = HTTPRequest{HTTPRequest::HTTP_PUT, getUri(requestString)};
            request.setContentType(MediaType{contentType});
            request.setContentLength(contentLength);
            logger << request.getMethod() << " " << request.getURI() << log_debug;
            return clientSession->sendRequest(request);
        }

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

        inline std::istream& putJsonString(const std::string& requestString, const std::string& jsonString, HTTPResponse& response)
        {
            logger << "putJsonString(" << requestString << ", " << jsonString << ")" << log_trace;
            auto& out = serverPut(requestString, "application/json", jsonString.size());
            out << jsonString;
            return clientSession->receiveResponse(response);
        }

        inline std::istream& putJsonObject(const std::string& requestString, Poco::JSON::Object::Ptr objPtr, HTTPResponse& response)
        {
            logger << "putJsonObject(" << requestString << ")" << log_trace;
            std::ostringstream oss;
            objPtr->stringify(oss);
            return putJsonString(requestString, oss.str(), response);
        }

        inline Poco::JSON::Object::Ptr dashboard()
        {
            logger << "dashboard()" << log_trace;
            return getJsonObject("/dashboard");
        }

        inline void detectorInit()
        {
            logger << "detectorInit()" << log_trace;
            HTTPResponse response;
            {
                auto& in = serverGet(std::string("/config/load?format=pixelconfig&file=") + bpcFilePath, response);
                checkResponse(response, in);
                logger << "Response of loading binary pixel configuration file: " << in.rdbuf() << log_notice;
                checkSession(in);
            }
            {
                auto& in = serverGet(std::string("/config/load?format=dacs&file=") + dacsFilePath, response);
                checkResponse(response, in);
                logger << "Response of loading dacs file: " << in.rdbuf() << log_notice;
                checkSession(in);
            }
        }

        inline Poco::JSON::Object::Ptr detectorConfig()
        {
            logger << "detectorConfig()" << log_trace;
            return getJsonObject("/detector/config");
        }

        inline Poco::JSON::Object::Ptr detectorInfo()
        {
            logger << "detectorInfo()" << log_trace;
            return getJsonObject("/detector/info");
        }

        void acquisitionInit(Poco::JSON::Object::Ptr configPtr, unsigned numTriggers, unsigned shutter_open_ms=490u, unsigned shutter_closed_ms=10u)
        {
            logger << "acquisitionInit(" << numTriggers << ", " << shutter_open_ms << ", " << shutter_closed_ms << ")" << log_trace;
            configPtr->set("nTriggers", numTriggers);
            configPtr->set("TriggerMode", "AUTOTRIGSTART_TIMERSTOP");
            configPtr->set("TriggerPeriod", (shutter_open_ms + shutter_closed_ms) / 1000.f);
            configPtr->set("ExposureTime", shutter_open_ms / 1000.f);

            HTTPResponse response;
            auto& in = putJsonObject("/detector/config", configPtr, response);
            checkResponse(response, in);
            logger << "Response of loading binary pixel configuration file: " << in.rdbuf() << log_notice;
            checkSession(in);
        }

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

        void acquisitionStart()
        {
            logger << "acquisitionStart()" << log_trace;
            HTTPResponse response;
            auto& in = serverGet("/measurement/start", response);
            checkResponse(response, in);
            logger << "Response of acquisition start: " << in.rdbuf() << log_notice;
            checkSession(in);
        }

        inline int main(const std::vector<std::string>& args) override
        {
            {
                auto log_proxy = logger << "main(";
                for (const auto& arg : args)
                    log_proxy << ' ' << arg;
                log_proxy << " )" << log_trace;
            }

            logger << "running on process " << Poco::Process::id() << log_info;

            if (stop)
                return rval;

            logger << "connecting to ASI server at " << serverAddress.toString() << log_notice;
            clientSession.reset(new HTTPClientSession{serverAddress});

            auto dashboardPtr = dashboard();
            std::string softwareVersion = (dashboardPtr|"Server")->getValue<std::string>("SoftwareVersion");
            {
                LogProxy log(logger);
                log << "Server Software Version: " << softwareVersion << "\nDashboard: ";
                dashboardPtr->stringify(log);
                log << log_notice;
            }

            detectorInit();

            {
                auto configPtr = detectorConfig();
                {
                    LogProxy log(logger);
                    log << "Response of getting the Detector Configuration from SERVAL: ";
                    configPtr->stringify(log);
                    log << log_notice;
                }

                unsigned numTriggers = 1;
                acquisitionInit(configPtr, numTriggers, 2, 950);
            }

            {
                auto infoPtr = detectorInfo();
                {
                    LogProxy log(logger);
                    log << "Response of getting the Detector Info from SERVAL: ";
                    infoPtr->stringify(log);
                    log << log_notice;
                }

                infoPtr->get("NumberOfChips").convert(numChips);
            }

            processing::init();

            logger << "listening at " << clientAddress.toString() << log_notice;
            serverSocket.reset(new ServerSocket{clientAddress});

            serverRawDestination(clientAddress);

            acquisitionStart();

            SocketAddress senderAddress;
            StreamSocket dataStream = serverSocket->acceptConnection(senderAddress);

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

            return Application::EXIT_OK;
        }

    public:
        explicit Tpx3App(Logger& log, int argc, char* argv[])
            : logger(log)
        {
            init(argc, argv);
        }

        inline virtual ~Tpx3App() {}
    };

} // namespace

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
