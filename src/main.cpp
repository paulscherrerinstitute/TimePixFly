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

        // serverip = 'localhost'
        // serverport = 8080
        // rawip = '127.0.0.1'
        // rawport = 8451
        SocketAddress serverAddress = SocketAddress{"localhost:8080"};  // ASI server address
        SocketAddress clientAddress = SocketAddress{"127.0.0.1:8451"};  // myself (raw data tcp destination)

        std::unique_ptr<HTTPClientSession> clientSession;   // client session with ASI server
        std::unique_ptr<ServerSocket> serverSocket;         // for connecting to myself

        Poco::JSON::Parser jsonParser;

        std::string bpcFilePath;
        std::string dacsFilePath;

        int64_t initialPeriod;
        unsigned long numBuffers = DEFAULT_NUM_BUFFERS;
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE;
        // unsigned long numAnalysers = DEFAULT_NUM_ANALYSERS;
        unsigned long numChips = 0;

    protected:
        inline void defineOptions(OptionSet& options) override
        {
            Application::defineOptions(options);

            options.addOption(Option("loglevel", "l")
                .description("log level: fatal,critical,error,warning,notice,information,debug,trace")
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
                .description("individual data buffer byte size, will be rounded up to a multiple of 8")
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

            // options.addOption(Option("num-analyzers", "t")
            //     .description("number of analyzer threads")
            //     .required(false)
            //     .repeatable(false)
            //     .argument("NUM")
            //     .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));
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
            // } else if (name == "num-analyzers") {
            //     if (num < 1)
            //         throw InvalidArgumentException{"non-positive number of analyser threads"};
            //     numAnalysers = num;
            } else {
                throw LogicException{std::string{"unknown number argument name: "} + name};
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

        // def tpx3_dashboard(serverurl):
        //     """Get a dashboard of the running TPX3 detector

        //     Keyword arguments:
        //     serverurl -- the URL of the running SERVAL (string)
        //     """
        inline Poco::JSON::Object::Ptr dashboard()
        {
            logger << "dashboard()" << log_trace;
            return getJsonObject("/dashboard");
        }

        // def tpx3_cam_init(serverurl, bpc_file, dacs_file):
        //     """Load TPX3 detector parameters required for operation, prints statuses

        //     Keyword arguments:
        //     serverurl -- the URL of the running SERVAL (string)
        //     bpc_file -- an absolute path to the binary pixel configuration file (string)
        //     dacs_file -- an absolute path to the text chips configuration file (string)
        //     """
        inline void detectorInit()
        {
            logger << "detectorInit()" << log_trace;
            HTTPResponse response;
            {
                //     # load a binary pixel configuration exported by SoPhy, the file should exist on the server
                //     resp = requests.get(url=serverurl + '/config/load?format=pixelconfig&file=' + bpc_file)
                //     data = resp.text
                //     print('Response of loading binary pixel configuration file: ' + data)
                auto& in = serverGet(std::string("/config/load?format=pixelconfig&file=") + bpcFilePath, response);
                checkResponse(response, in);
                logger << "Response of loading binary pixel configuration file: " << in.rdbuf() << log_notice;
                checkSession(in);
            }
            {
                //     #  .... and the corresponding DACS file
                //     resp = requests.get(url=serverurl + '/config/load?format=dacs&file=' + dacs_file)
                //     data = resp.text
                //     print('Response of loading dacs file: ' + data)
                auto& in = serverGet(std::string("/config/load?format=dacs&file=") + dacsFilePath, response);
                checkResponse(response, in);
                logger << "Response of loading dacs file: " << in.rdbuf() << log_notice;
                checkSession(in);
            }
        }

        // # Example of getting the detector configuration from the server in JSON format
        // resp = requests.get(url=serverurl + '/detector/config')
        // data = resp.text
        // print('Response of getting the Detector Configuration from SERVAL: ' + data)
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

        // def tpx3_acq_init(serverurl, detector_config, ntrig=1, shutter_open_ms=490, shutter_closed_ms=10):
        //     """Set the number of triggers and the shutter timing for TPX3 detector

        //     Keyword arguments:
        //     serverurl -- the URL of the running SERVAL (string)
        //     detector_config -- a dictionary with Detector Configuration data (Python dictionary)
        //     ntrig -- number of triggers to be executed (integer, default value is 1)
        //     shutter_open_ms -- open time of the shutter in milliseconds (integer, default value is 490)
        //     shutter_closed_ms -- closed time of the shutter in milliseconds (integer, default value is 10)
        //     """
        void acquisitionInit(Poco::JSON::Object::Ptr configPtr, unsigned numTriggers, unsigned shutter_open_ms=490u, unsigned shutter_closed_ms=10u)
        {
            logger << "acquisitionInit(" << numTriggers << ", " << shutter_open_ms << ", " << shutter_closed_ms << ")" << log_trace;
            //     # Sets the number of triggers.
            //     detector_config["nTriggers"] = ntrig
            configPtr->set("nTriggers", numTriggers);

            //     # Set the trigger mode to be software-defined.
            //     detector_config["TriggerMode"] = "AUTOTRIGSTART_TIMERSTOP"
            configPtr->set("TriggerMode", "AUTOTRIGSTART_TIMERSTOP");

            //     # Sets the trigger period (time between triggers) in seconds.
            //     detector_config["TriggerPeriod"] = (shutter_open_ms + shutter_closed_ms) / 1000
            configPtr->set("TriggerPeriod", (shutter_open_ms + shutter_closed_ms) / 1000.f);

            //     # Sets the exposure time (time the shutter remains open) in seconds.
            //     detector_config["ExposureTime"] = shutter_open_ms / 1000
            configPtr->set("ExposureTime", shutter_open_ms / 1000.f);

            //     # Upload the Detector Configuration defined above
            //     resp = requests.put(url=serverurl + '/detector/config', data=json.dumps(detector_config))
            //     data = resp.text
            //     print('Response of updating Detector Configuration: ' + data)
            HTTPResponse response;
            auto& in = putJsonObject("/detector/config", configPtr, response);
            checkResponse(response, in);
            logger << "Response of loading binary pixel configuration file: " << in.rdbuf() << log_notice;
            checkSession(in);
        }

        // # Example of destination configuration (Python dictionary) for the data output
        // destination = {
        //     "Raw": [{
        //         "Base": "tcp://connect@" + rawip + ":" + str(rawport),
        //     }]
        // }
        // # Setting destination for the data output
        // resp = requests.put(url=serverurl + '/server/destination', data=json.dumps(destination))
        // data = resp.text
        // print('Response of uploading the Destination Configuration to SERVAL : ' + data)
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

        // def tpx3_simple_acq(serverurl):
        //     """Perform TPX3 detector acquisition

        //     Keyword arguments:
        //     serverurl -- the URL of the running SERVAL (string)
        //     """
        //     resp = requests.get(url=serverurl + '/measurement/start')
        //     data = resp.text
        //     print('Response of acquisition start: ' + data)
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

            // serverurl = 'http://' + serverip + ':' + str(serverport)
            // dashboard = tpx3_dashboard(serverurl)
            // # Print selected parameter from the TPX3 detector dashboard
            // print('Server Software Version:', dashboard['Server']['SoftwareVersion'])
            // # Print whole TPX3 detector dashboard
            // print('Dashboard:', dashboard)
            auto dashboardPtr = dashboard();
            std::string softwareVersion = (dashboardPtr|"Server")->getValue<std::string>("SoftwareVersion");
            {
                LogProxy log(logger);
                log << "Server Software Version: " << softwareVersion << "\nDashboard: ";
                dashboardPtr->stringify(log);
                log << log_notice;
            }

            // # Change file paths to match the paths on the server:
            // # For example, a Linux path:
            // # bpcFile = '/home/asi/tpx3-demo.bpc
            // # dacsFile = '/home/asi/tpx3-demo.bpc.dacs'
            // # And on Windows:
            // # bpcFile = 'C:\\Users\\ASI\\tpx3-demo.bpc'
            // # dacsFile = 'C:\\Users\\ASI\\tpx3-demo.bpc.dacs'
            // # The following requires the files to be in the working directory:
            // bpcFile = os.path.join(os.getcwd(), 'tpx3-demo.bpc')
            // dacsFile = os.path.join(os.getcwd(), 'tpx3-demo.dacs')
            // # Detector initialization with bpc and dacs
            detectorInit();

            {
                auto configPtr = detectorConfig();
                {
                    LogProxy log(logger);
                    log << "Response of getting the Detector Configuration from SERVAL: ";
                    configPtr->stringify(log);
                    log << log_notice;
                }

                // // # Converting detector configuration data from JSON to Python dictionary and modifying values
                // // detectorConfig = json.loads(data)
                // // detectorConfig["BiasVoltage"] = 100
                // // detectorConfig["BiasEnabled"] = True
                // configPtr->set("BiasVoltage", 100);
                // configPtr->set("BiasEnabled", true);

                // # Setting triggers and timing for the acquisition
                // numTriggers = 1
                // tpx3_acq_init(serverurl, detectorConfig, numTriggers, 2, 950)
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

            // {
            //     // # Getting the updated detector configuration  from SERVAL
            //     // resp = requests.get(url=serverurl + '/detector/config')
            //     // data = resp.text
            //     // print('Response of getting the updated Detector Configuration from SERVAL : ' + data)
            //     auto configPtr = detectorConfig();
            //     {
            //         LogProxy log(logger);
            //         log << "Response of getting the updated Detector Configuration from SERVAL: ";
            //         configPtr->stringify(log);
            //         log << log_notice;
            //     }
            // }

            // def create_socket(host, port):
            //     socket_ = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            //     socket_.bind((host, port))
            //     socket_.listen()
            //     return socket_
            // socket_ = create_socket(rawip, rawport)
            logger << "listening at " << clientAddress.toString() << log_notice;
            serverSocket.reset(new ServerSocket{clientAddress});

            serverRawDestination(clientAddress);

            // resp = requests.get(url=serverurl + '/measurement/start')
            // data = resp.text
            // print('Response of starting measurement:', data)
            acquisitionStart();

            SocketAddress senderAddress;
            StreamSocket dataStream = serverSocket->acceptConnection(senderAddress);

            const auto t1 = wall_clock::now();

            logger << "connection from " << senderAddress.toString() << log_info;

            DataHandler<AsiRawStreamDecoder> dataHandler(dataStream, logger, bufferSize, numChips, initialPeriod);
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
