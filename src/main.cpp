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

#include <cstdlib>
#include <csignal>
#include <cerrno>
#include <limits>
#include <fcntl.h>
#include <cerrno>

#include <sys/file.h>
#include <poll.h>

#include "Poco/Util/OptionCallback.h"
#include "Poco/Dynamic/Var.h"
#include "Poco/JSON/Parser.h"
#include "Poco/Util/Application.h"
#include "Poco/Util/OptionSet.h"
#include "Poco/Util/HelpFormatter.h"
#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/MediaType.h"
#include "Poco/Process.h"
#include "Poco/Timespan.h"
#include "Poco/SyslogChannel.h"

#include "json_ops.h"
#include "config_file.h"
#include "data_handler.h"
#include "copy_handler.h"
#include "rest_callbacks.h"


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

    #include "version.h"

    //=========================
    // Lock file
    //=========================

    extern "C" {

        /*!
        \brief Handle SIGINT(CTRL-C), SIGTERM, SIGHUP, SIGQUIT, SIGUSR1
        All signals stop the server/receiver loop, SIGUSR1 restarts the server
        \param sig Signal number
        */
        inline static void sigint_handler([[maybe_unused]] int sig)
        {
            if (sig == SIGUSR1)
                global::instance->restart = true;
            global::instance->stop = true;
        }
    }

    /*!
    \brief Lock file to prevent double instances
    */
    struct Lockfile final {
        /*!
        \brief Constructor
        Create pid file and lock it exclusively.
        \param logger Logger
        */
        inline explicit Lockfile(Logger& logger)
            : log(logger)
        {
            if (lock_file == "none")
                return;
            log << "open pid file at " << lock_file << log_debug;
            fd = open(lock_file.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd < 0) {
                if (errno == EEXIST) {
                    throw Poco::RuntimeException(std::string{"lockfile exists at "} + lock_file + ", is another tpx2app already running?");
                } else {
                    throw Poco::RuntimeException(std::string{"unable to create lockfile at "} + lock_file);
                }
            }
            if (flock(fd, LOCK_EX) != 0) {
                close(fd);
                fd = -1;
                throw Poco::RuntimeException(std::string{"unable to lock file at "} + lock_file + ", is another tpx2app already running?");
            }
            std::atexit(Lockfile::atexit);
            std::ostringstream oss;
            oss << getpid() << '\n';
            auto pids = oss.str();
            if (write(fd, pids.data(), pids.size()) != (ssize_t)pids.size())
                log << "unable to write pid into lockfile at " << lock_file << log_warn;
        }

        /*!
        \brief Destructor
        Unlink pid file
        */
        inline ~Lockfile()
        {
            if (fd >= 0) {
                log << "unlink pid file at " << lock_file << log_debug;
                atexit();
            }
        }

        inline static std::string lock_file = "/tmp/tpx3app.pid";   //!< Lock file name or "none"

    private:

        /*!
        \brief Atexit handler
        Unlink the pid file
        */
        inline static void atexit() noexcept
        {
            if (fd >= 0) {
                if (unlink(lock_file.c_str()))
                    std::cerr << "Error: unlink " << lock_file.c_str() << " - " << strerror(errno) << '\n';
                if (flock(fd, LOCK_UN))
                    std::cerr << "Error: unlocking lock file - " << strerror(errno) << '\n';
                if (close(fd))
                    std::cerr << "Error: closing lock_file - " << strerror(errno) << '\n';
                fd = -1;
            }
        }

        Logger& log;    //!< Logger
        inline static int fd = -1;    //!< File descriptor
    };

    //=========================
    // Main Poco Application
    //=========================

    /*!
    \brief Poco object for TPX3 raw stream analysis application
    */
    class Tpx3App final : public Application {

        constexpr static unsigned DEFAULT_BUFFER_SIZE = 8 * 4096;   //!< Default IO buffer size
        // constexpr static unsigned DEFAULT_NUM_ANALYSERS = 6;

        Logger& logger;                 //!< Poco::Logger object
        int rval = Application::EXIT_OK;//!< Default application return value

        std::unique_ptr<HTTPClientSession> clientSession;   //!< Client session with ASI server
        std::unique_ptr<ServerSocket> serverSocket;         //!< Socket for connecting to myself

        Poco::JSON::Parser jsonParser;  //!< Poco JSON parser

        std::string bpcFilePath;        //!< Path to ASI bpc detector configuration file (optional)
        std::string dacsFilePath;       //!< Path to ASI dacs detector configuration file (optional)
        std::string streamFilePath;     //!< Path (and flag) to file to which the raw event stream should be copied (don't copy if empty)

        // static constexpr float ns_to_clk = 2. / 3.125;  //!< Nanoseconds to TDC timestamp clock ticks (see ASI server TDC event description, only 1 bit of fine timestamp used) (currently unused)
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE; //!< IO buffer size
        // unsigned long numAnalysers = DEFAULT_NUM_ANALYSERS;
        unsigned long reorderQueueSize = 64;            //!< Number of elements in the event reorder queue
        unsigned numChips = 0;                          //!< Number of TPX3 chips on the detector

    protected:
        /*!
        \brief Poco application options definition
        \param options Poco options set
        */
        inline void defineOptions(OptionSet& options) override
        {
            Application::defineOptions(options);
            auto& gvars = *global::instance;

            options.addOption(Option("loglevel", "l")
                .description("log level:\nfatal,critical,error,warning,\n"
                             "notice,information,debug,trace\ndefault: critical")
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
                .description(std::string{"ASI server address\ndefault: "} + gvars.serverAddress.toString())
                .required(false)
                .repeatable(false)
                .argument("ADDRESS")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleAddress)));

            options.addOption(Option("address", "a")
                .description(std::string{"my address\ndefault: "} + gvars.clientAddress.toString())
                .required(false)
                .repeatable(false)
                .argument("ADDRESS")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleAddress)));

            options.addOption(Option("control", "c")
                .description(std::string{"control interface address\ndefault: "} + gvars.controlAddress.toString())
                .required(false)
                .repeatable(false)
                .argument("ADDRESS")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleAddress)));

            options.addOption(Option("bpc-file", "b")
                .description("optional bpc file path")
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFilePath)));

            options.addOption(Option("dacs-file", "d")
                .description("optional dacs file path")
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFilePath)));

            options.addOption(Option("buf-size", "N")
                .description(std::string{"individual data buffer byte size,\n"
                             "will be rounded up to a multiple of the system page_size\ndefault: "} + std::to_string(DEFAULT_BUFFER_SIZE))
                .required(false)
                .repeatable(false)
                .argument("NUM")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));

            options.addOption(Option("reorder-queue-size", "q")
                .description(std::string{"number of reorder queue elements\ndefault: "} + std::to_string(reorderQueueSize))
                .required(false)
                .repeatable(false)
                .argument("NUM")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));

            options.addOption(Option("save-interval", "i")
                .description(std::string{"writeout period in TDC periods,\n"
                             "0 for infinite.\ndefault: "} + std::to_string(global::instance->save_interval))
                .required(false)
                .repeatable(false)
                .argument("PERIODS")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleNumber)));

            options.addOption(Option("stream-to-file", "f")
                .description("stream to file, \"none\" for read only")
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleFilePath)));

            options.addOption(Option("server-mode", "S")
                .description("run in server-mode")
                .required(false)
                .repeatable(false)
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleBool)));

            options.addOption(Option("use-syslog", "L")
                .description(std::string{"use syslog for logging with\nSYSLOG_IDENTIFIER="} + NAME)
                .required(false)
                .repeatable(false)
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleBool)));

            options.addOption(Option("version", "v")
                .description(std::string{"show version\nversion: "} + VERSION)
                .required(false)
                .repeatable(false)
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleVersion)));

            options.addOption(Option("pid-file", "P")
                .description(std::string{"pid file for locking, or 'none'\ndefault: "} + Lockfile::lock_file)
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handlePidfile)));
            options.addOption(Option("config-file", "C")
                .description("ini style config file for arguments.\n"
                             "commandline: --xy=val\n"
                             "ini file   : xy=val\n"
                             "The file takes precedence over previous commandline arguments, \n"
                             "but the following commandline arguments take precedence over the file.")
                .required(false)
                .repeatable(false)
                .argument("PATH")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleConfigFile)));
            options.addOption(Option("cpu-affinity", "A")
                .description("cpu affinity setting\n"
                             "affinity = (a|w|r):cpu-set OR affinity;affinity\n"
                             "cpu-set = cpu OR range OR cpu-set,cpu-set\n"
                             "range = cpu-cpu")
                .required(false)
                .repeatable(false)
                .argument("CPUSET")
                .callback(OptionCallback<Tpx3App>(this, &Tpx3App::handleCpuAffinity)));
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
            helpFormatter.setUsage(commandName() + " OPTIONS\n       " + commandName() + " (stop | restart) [ini/pid-file]\n");
            helpFormatter.setHeader(std::string{"Handle TimePix3 raw stream or send SIGTERM (for stop) or SIGUSR1 (for restart)\n"
                                    "to the tpx3app process with the PID recorded in the pid-file. The default\n"
                                    "pid-file is "} + Lockfile::lock_file);
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
            logger << "handleBool(" << name << ", " << value << ")" << log_trace;
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
            if (name == "server-mode") {
                global::instance->server_mode = val;
            } else if (name == "use-syslog") {
                if (val) {
                    using Poco::SyslogChannel;
                    Poco::AutoPtr<SyslogChannel> schan{new SyslogChannel};
                    schan->setProperty("name", logger.name());
                    logger << "set loggin channel to syslog" << log_debug;
                    logger.setChannel(schan);
                }
            } else {
                throw LogicException{std::string{"unknown bool argument name: "} + name};
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
                if (num < 1024)
                    throw InvalidArgumentException{"buffer size too small"};
                auto page_size = sysconf(_SC_PAGE_SIZE);
                bufferSize = ((num + page_size - 1) / page_size) * page_size;
            } else if (name == "reorder-queue-size") {
                if (num < 16)
                    throw InvalidArgumentException{"reorder queue size is too small"};
                reorderQueueSize = num;
            } else if (name == "save-interval") {
                period_type max = std::numeric_limits<period_type>::max();
                if (num == 0)
                    num = max;
                if ((period_type)num < 100)
                    throw InvalidArgumentException{"save-interval should be at least 100"};
                global::instance->save_interval = num;
            } else {
                throw LogicException{std::string{"unknown number argument name: "} + name};
            }
        }

        // Currently unused
        // /* TODO !
        // \brief Real valued option handler
        // \param name     Option name
        // \param value    Option value
        // */
        // inline void handleFloat(const std::string& name, const std::string& value)
        // {
        //     logger << "handleFloat(" << name << ", " << value << ")" << log_trace;
        //     double val = .0;
        //     try {
        //         val = std::stod(value);
        //     } catch (std::exception& ex) {
        //         throw InvalidArgumentException{std::string{"invalid value for argument: "} + name};
        //     }
        //     if (name == "undisputed-threshold") {
        //         if ((val < .0) || (val > .5))
        //             throw InvalidArgumentException{"undisputed-period outside of [0 .. 0.5]"};
        //         undisputedThreshold = val;
        //     } else {
        //         throw LogicException{std::string{"unknown float argument name: "} + name};
        //     }
        // }

        /*!
        \brief IP address option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handleAddress(const std::string& name, const std::string& value)
        {
            logger << "handleAddress(" << name << ", " << value << ')' << log_trace;
            auto& gvars = *global::instance;

            if (name == "server") {
                try {
                    gvars.serverAddress = SocketAddress{value};
                } catch (Poco::Exception& ex) {
                    throw InvalidArgumentException{"server address", ex, __LINE__};
                }
            } else if (name == "address") {
                try {
                    gvars.clientAddress = SocketAddress{value};
                } catch (Poco::Exception& ex) {
                    throw InvalidArgumentException{"my address", ex, __LINE__};
                }
            } else if (name == "control") {
                try {
                    gvars.controlAddress = SocketAddress(value);
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
        \brief Pid file option handler
        \param name     Option name
        \param value    Option value
        */
        inline void handlePidfile([[maybe_unused]] const std::string& name, const std::string& value)
        {
            logger << "handlePidfile(" << name << ", " << value << ')' << log_trace;
            Lockfile::lock_file = value;
        }

        /*!
        \brief Ini style config file handler
        The config file should have the .ini suffix.
        The config file defines arguments like
        ```
        use-syslog=true
        buf-size=1024
        ```
        use-syslog and loglevel are handled first.
        \param name     Option name - "config-file"
        \param value    Option value - path to the ini style file
        */
        inline void handleConfigFile([[maybe_unused]] const std::string& name, const std::string& value)
        {
            logger << "handleConfigFile(" << name << ", " << value << ')' << log_trace;
            try {
                if (value.find(".ini") == std::string::npos)
                    throw InvalidArgumentException("config file must have the .ini suffix");

                ConfigFile cf{value};

                {
                    // handle use syslog and loglevel first
                    if (cf.getBool("use-syslog", false))
                        handleBool("use-syslog", "1");
                    std::string ll = cf.getString("loglevel", "");
                    if (!ll.empty())
                        handleLogLevel("loglevel", ll);
                }
                {
                    std::string argstr;
                    argstr = cf.getString("server", "");
                    if (!argstr.empty())
                        handleAddress("server", argstr);
                    argstr = cf.getString("address", "");
                    if (!argstr.empty())
                        handleAddress("address", argstr);
                    argstr = cf.getString("control", "");
                    if (!argstr.empty())
                        handleAddress("control", argstr);
                    argstr = cf.getString("bpc-file", "");
                    if (!argstr.empty())
                        handleFilePath("bpc-file", argstr);
                    argstr = cf.getString("dacs-file", "");
                    if (!argstr.empty())
                        handleFilePath("dacs-file", argstr);
                    argstr = cf.getString("stream-to-file", "");
                    if (!argstr.empty())
                        handleFilePath("stream-to-file", argstr);
                    argstr = cf.getString("pid-file", "");
                    if (!argstr.empty())
                        handlePidfile("pid-file", argstr);
                }
                {                
                    std::uint32_t argint;
                    argint = cf.getUInt("buf-size", 0);
                    if (argint > 0)
                        handleNumber("buf-size", std::to_string(argint));
                    argint = cf.getUInt("reorder-queue-size", 0);
                    if (argint > 0)
                        handleNumber("reorder-queue-size", std::to_string(argint));
                }
                {
                    bool argb;
                    argb = cf.getBool("server-mode", false);
                    if (argb)
                        handleBool("server-mode", std::to_string(argb));
                }
            } catch (Poco::Exception& ex) {
                throw InvalidArgumentException{std::string{"bad config file - "} + ex.message()};
            }
        }

        /*!
        \brief CPU affinity setting handler
        \param name     Option name - "cpu-affinity"
        \param value    Option value - cpu affinity setting, like a:0-7;r:8;w:9
        */
        inline void handleCpuAffinity([[maybe_unused]] const std::string& name, const std::string& value)
        {
            logger << "handleConfigFile(" << name << ", " << value << ')' << log_trace;
            auto& mask = global::instance->cpu_affinity;
            cpu_mask::parse(mask, value, [](unsigned pos, const std::string& errmsg) {
                throw InvalidArgumentException(std::string{"cpu-affinity - "} + errmsg + " at position " + std::to_string(pos));
            });
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
            try {
                auto request = HTTPRequest{HTTPRequest::HTTP_GET, getUri(requestString)};
                logger << request.getMethod() << " " << request.getURI() << log_debug;
                clientSession->sendRequest(request);
                return clientSession->receiveResponse(response);
            } catch (Poco::Exception& ex) {
                throw Poco::RuntimeException{std::string{"ASI server GET request for " + requestString + " failed - " + ex.displayText()}};
            }
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
            try {
                auto request = HTTPRequest{HTTPRequest::HTTP_PUT, getUri(requestString)};
                request.setContentType(MediaType{contentType});
                request.setContentLength(contentLength);
                logger << request.getMethod() << " " << request.getURI() << log_debug;
                return clientSession->sendRequest(request);
            } catch (Poco::Exception& ex) {
                throw Poco::RuntimeException{std::string{"ASI server PUT request for " + requestString + " failed - " + ex.displayText()}};
            }
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
        \brief Set program state with log message
        \param state New program state
        */
        inline void set_state(const std::string_view& state)
        {
            logger << "new state: " << state << log_debug;
            StateHandler::set_state(state);
        }

        /*!
        \brief Check parameter consistency
        \throw InvalidArgumentException on detected inconsistencies
        */
        void checkParameterConsistency()
        {
            auto& gvars = *global::instance;
            if (gvars.clientAddress == gvars.controlAddress)
                throw InvalidArgumentException("--address and --control parameters must be different");
            if (gvars.clientAddress == gvars.serverAddress)
                throw InvalidArgumentException("--address and --server parameters must be different");
            if (gvars.controlAddress == gvars.serverAddress)
                throw InvalidArgumentException("--server and --control parameters must be different");
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

            checkParameterConsistency();

            logger << "running on process " << Poco::Process::id() << log_info;

            auto& gvars = *global::instance;
            if (gvars.stop)
                return rval;

            iobuf::container_size = bufferSize;
            const bool server_mode = gvars.server_mode;

            // ----------------------- get detector server data -----------------------

            logger << "connecting to ASI server at " << gvars.serverAddress.toString() << log_notice;
            clientSession.reset(new HTTPClientSession{gvars.serverAddress});

            {
                auto dashboardPtr = dashboard();
                std::string softwareVersion = (dashboardPtr|"Server")->getValue<std::string>("SoftwareVersion");
                {
                    LogProxy log(logger);
                    log << "Server Software Version: " << softwareVersion << "\nDashboard: ";
                    dashboardPtr->stringify(log.base());
                    log << log_notice;
                }
            }

            if (! server_mode)
                detectorInit();

            {
                auto configPtr = detectorConfig();
                {
                    LogProxy log(logger);
                    log << "Response of getting the Detector Configuration from SERVAL: ";
                    configPtr->stringify(log.base());
                    log << log_notice;
                }

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

            detector_layout& layout = global::instance->layout;
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

            rest::init_callbacks();
            auto restService = rest::start_service(logger, gvars.controlAddress);

            std::unique_ptr<DataHandler<AsiRawStreamDecoder>> dataHandlerPtr; 
            if (streamFilePath.empty()) {
                // if not copy mode, create the data handler in advance
                dataHandlerPtr.reset(new DataHandler<AsiRawStreamDecoder>{logger, numChips, reorderQueueSize});
            }

            do {
                if (! global::instance->last_error.empty())
                    set_state(global::except);

                if (global::instance->server_mode) { // wait for start signal
                    using namespace std::chrono_literals;
                    set_state(global::config);
                    while (!global::instance->stop && !global::instance->start) {
                        std::this_thread::sleep_for(1ms);
                    }
                    if (global::instance->stop) {
                        global::instance->last_error.clear();
                        break; // exit server mode loop
                    }
                    global::instance->start = false;
                }

                global::instance->stop_collect = false;
                set_state(global::setup);

                try {
                    if (streamFilePath.empty())
                        processing::init(layout);

                    logger << "listening at " << gvars.clientAddress.toString() << log_notice;
                    serverSocket.reset(new ServerSocket{gvars.clientAddress});
                    serverSocket->setReuseAddress(true);
                    serverSocket->setReusePort(true);

                    serverRawDestination(gvars.clientAddress);

                    if (! global::instance->server_mode)
                        acquisitionStart();

                    SocketAddress senderAddress;
                    set_state(global::await_connection);
                    //------------------------------------------------
                    // accept connection from ASI server using poll with a timeout
                    {
                        int fd = serverSocket->impl()->sockfd();

                        // pollfd structure
                        struct pollfd fds[1];
                        fds[0].fd = fd;
                        fds[0].events = POLLIN; // Check for incoming data (connection)

                        // timeout in ms for the poll call
                        int timeout = std::max(global::instance->collect_timeout / 1000u, 10u);
                        int ret = 0;

                        do {
                            ret = poll(fds, 1, timeout);

                            if (ret == -1) {
                                throw Poco::RuntimeException(std::string{"poll failed - "} + std::strerror(errno));
                            } else if (ret == 0) {  // timeout
                                if (global::instance->stop_collect)
                                    break;
                            } else if (fds[0].revents & POLLIN) {
                                break;
                            }
                        } while (true);

                        if (ret == 0)               // stop_collect == true
                            continue;
                    }
                    StreamSocket dataStream = serverSocket->acceptConnection(senderAddress);
                    //------------------------------------------------
                    set_state(global::collect);
                    dataStream.setReceiveTimeout(global::instance->collect_timeout);

                    if (! streamFilePath.empty()) {
                        Timer timer;

                        CopyHandler copyHandler(dataStream, streamFilePath, logger);
                        global::instance->stop_handlers.emplace_back([&copyHandler]() {
                            copyHandler.stopNow();
                        });

                        copyHandler.run_async();
                        copyHandler.await();

                        const double time = timer.elapsed();

                        const auto items = copyHandler.writeTotalBytes / sizeof(u64);
                        const auto ri = copyHandler.readTotalBytes / sizeof(u64);
                        const auto wi = copyHandler.writeTotalBytes / sizeof(u64);
                        const auto rt = copyHandler.readTime;
                        const auto rot = copyHandler.readOpTime;
                        const auto wt = copyHandler.writeTime;
                        const auto wot = copyHandler.writeOpTime;

                        logger << "total: " << items << " items in " << time << "s at " << (items / time) << " items/s\n"
                               << "read: " << ri << " items in " << rt << "s at " << (ri / rt) << " items/s, op: " << rot << "s at " << (ri / rot) << " items/s\n"
                               << "write: " << items << " items in " << wt << "s at " << (wi / wt) << " items/s, op: " << wot << "s at " << (wi / wot) << " items/s" << log_notice;
                    } else {
                        auto& dataHandler = *dataHandlerPtr;
                        Timer timer;

                        logger << "connection from " << senderAddress.toString() << log_info;

                        dataHandler.rawDataStream(dataStream);
                        global::instance->stop_handlers.emplace_back([&dataHandler]() {
                            dataHandler.stopNow();
                        });

                        dataHandler.run_async();
                        dataHandler.await();

                        const double time = timer.elapsed();

                        dataStream.close();

                        const uint64_t ntoa = dataHandler.toaCount;
                        const uint64_t ntdc = dataHandler.tdcCount;
                        const u64 readCount = (dataHandler.byteCount / sizeof(u64));
                        const double avgAnalysisWorkTime = dataHandler.analyseWorkTime / numChips;
                        const double avgAnalysisTime = (dataHandler.analyseWorkTime + dataHandler.analyseSpinTime) / numChips;
                        logger << "time: " << time << "s tdcs: " << ntdc << " toas: " << ntoa << " at " << (ntoa / time)
                                                   << " toas/s rate: " << ((ntoa+ntdc) / time) << " events/s\n"
                            << "analysis spin: " << dataHandler.analyseSpinTime << "s work 1:" << dataHandler.analysePassOneTime
                                                                                       << " 2:" << dataHandler.analysePassTwoTime
                                                                                       << " 3:" << dataHandler.analysePassThreeTime
                                                                                       << " self: " << dataHandler.analyseWorkTime
                                                                                       << " avg: " << avgAnalysisWorkTime
                            << "\n         self rate: " << (ntoa / avgAnalysisWorkTime) << " toas/s " << ((ntoa+ntdc) / avgAnalysisWorkTime) << " events/s"
                            << "\n         rate: " << (ntoa / avgAnalysisTime) << " toas/s " << ((ntoa+ntdc) / avgAnalysisTime) << " events/s"
                            << "\nreading spin: " << dataHandler.readSpinTime << "s work: " << dataHandler.readTime
                                                  << "s total: " << dataHandler.readTotalTime << "s items: " << readCount
                                                  << " at " << (readCount / dataHandler.readTotalTime) << " items/s"
                                                  << ", " << (ntoa / dataHandler.readTotalTime) << " toas/s"
                                                  << ", " << ((ntoa+ntdc) / dataHandler.readTotalTime) << " events/s" << log_notice;
                    }
                } catch (Poco::Exception& ex) {
                    global::instance->last_error = ex.displayText();
                    set_state(global::except);
                    LogProxy log(logger);
                    log << ex.displayText() << '\n';
                    const Poco::Exception* pEx = & ex;
                    while ((pEx = pEx->nested())) {
                        log << "  " << pEx->displayText() << '\n';
                    }
                    log << log_critical;
                } catch (std::exception& ex) {
                    global::instance->last_error = ex.what();
                    set_state(global::except);
                    logger << "Exception: " << ex.what() << log_critical;
                }
            } while (global::instance->server_mode && !global::instance->stop);

            if (! global::instance->last_error.empty()) {
                set_state(global::except);
                logger << "Exception: " << global::instance->last_error << log_error;
            }

            set_state(global::shutdown);
            StateHandler::stop();
            rest::stop_service(restService.get());

            if (global::instance->last_error.empty())
                return Application::EXIT_OK;
            return Application::EXIT_USAGE;
        }

    public:
        /*!
        \brief Constructor
        \param log  Poco::Logger object
        \param argc Number of commandline arguments
        \param argv Commandline argument values
        */
        inline explicit Tpx3App(Logger& log, int argc, char* argv[])
            : logger(log)
        {
            init(argc, argv);
        }

        inline virtual ~Tpx3App() {}                //!< Destructor

        constexpr static char NAME[] = "Tpx3App";   //!< Name (e.g. for syslog)
    };

    /*!
    \brief Handle stop and restart command
    \param argc Number of comandline arguments
    \param argv Commandline arguments
    */
    void stopOrRestartCommand(int argc, char* argv[])
    {
        // Search for "stop" or "restart"
        int command = 0;
        for (int i=1; i<argc; i++) {
            auto arg = std::string_view(argv[i]);
            if ((arg == "stop") || (arg == "restart")) {
                command = i;
                break;
            }
        }

        // Handle command and exit, or just leave it
        if (command != 0) {
            int retval = Application::EXIT_OK;
            try {
                int pid = 0;
                const std::string cmd{argv[command]};
                if (command != 1)
                    throw InvalidArgumentException(cmd + " must be the first argument");
                if (argc > 3)
                    throw InvalidArgumentException(std::string{"only one argument (lockfile) is allowed for "} + cmd);
                {
                    std::string pid_file = argc > 2 ? std::string{argv[2]} : Lockfile::lock_file;
                    if (pid_file.rfind(".ini") != std::string::npos) {
                        ConfigFile conf{pid_file};
                        try {
                            pid_file = conf.getString("pid-file");
                        } catch (Poco::NotFoundException&) {
                            throw InvalidArgumentException(std::string{"unable to find key pid-file in ini file "} + pid_file);
                        }
                    }
                    std::ifstream ifs(pid_file);
                    ifs >> pid;
                    if (!ifs || (pid==0))
                        throw InvalidArgumentException(std::string{"unable to read pid from "} + pid_file + " (is tpx3app running?)");
                }
                bool stop = (cmd == "stop");
                std::cout << (stop ? "stop" : "restart") << " process " << pid << '\n';
                int res = kill(pid, stop ? SIGTERM : SIGUSR1);
                if (res != 0)
                    throw RuntimeException(std::strerror(errno));
            } catch (Poco::Exception& ex) {
                std::cerr << ex.displayText() << '\n';
                retval = Application::EXIT_USAGE;
            } catch (...) {
                std::cerr << "internal error\n";
                retval = Application::EXIT_SOFTWARE;
            }
            std::exit(retval);
        }
    }

} // namespace

/*!
\brief Entry function
\param argc Number of commandline parameters
\param argv Values of commandliine parameters
\return 0 if no error, not 0 otherwise
*/
int main (int argc, char* argv[])
{
    int retval = Application::EXIT_USAGE;

    try {
        Logger& logger = Logger::get(Tpx3App::NAME);
        LogProxy log(logger);

        stopOrRestartCommand(argc, argv); // exits if applicable

        std::signal(SIGINT, sigint_handler);
        std::signal(SIGTERM, sigint_handler);
        std::signal(SIGHUP, sigint_handler);
        std::signal(SIGQUIT, sigint_handler);
        std::signal(SIGUSR1, sigint_handler);

        try {
            retval = Application::EXIT_SOFTWARE;
            logger.setLevel(Message::PRIO_CRITICAL);
            Tpx3App app(logger, argc, argv);
            if (global::instance->stop)
                return Application::EXIT_OK;
            Lockfile pid_file{logger};

            do {
                global::instance->stop.store(false);
                global::instance->restart.store(false);
                retval = app.run();
                if (global::instance->restart.load())
                    log << "restart, last error is \"" << global::instance->last_error << '"' << log_info;
                else
                    break;
            } while (true);

        } catch (Poco::Exception& ex) {
            log << ex.displayText() << '\n';
            const Poco::Exception* pEx = & ex;
            while ((pEx = pEx->nested())) {
                log << "  " << pEx->displayText() << '\n';
            }
            log << log_fatal;
        } catch (std::exception& ex) {
            log << "Exception: " << ex.what() << log_fatal;
        }

    } catch (Poco::Exception& ex) {
        std::cerr << "fatal error: " << ex.displayText() << '\n';
    } catch (std::exception& ex) {
        std::cerr << "fatal error: " << ex.what() << '\n';
    }

    return retval;
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

- a main thread (see main.cpp) that interacts with the ASI server and REST client
- a reader thread (see data_handler.h) that reads the TCP raw event data stream produced by the ASI server
- per chip analysis threads (see data_handler.h) that dispatch events from the raw stream, build and write per chip histograms
- a writer thread (see xes_data_manager.h) that aggregates per chip histograms and writes aggregated histograms out to disk or the network

The incoming data stream is put into jars (see io_buf.h) by the reader thread.

\image html io_buf.png width=80%

The raw event stream from the ASI server comes in packets per chip. Every analysis thread skips to the packets for the chip the thread is
associated with. Per chip there's a single analysis thread that dispatches events from packets within the jars that contain events for
the associated chip.

Every analysis thread deals with event data originating from a single detector chip by putting them into a priority queue of a fixed size.
The priority queue reorders the events according to time.

\image html priority_queue.png width=80%

As soon as the reordering priority queue is full, events are sent to the histogramming code (see processing.cpp). The per chip histogram
is periodically (see save_interval) pushed to a period queue (see xes_data_manager.h), where the writer thread picks
it up, combines it with other per chip histograms for the same period and writes the final histogram data out to disk or network.

\image html output_queue.png width=80%

Output is either in files per save period (see safe_interval) or TCP streamed JSON data per safe period (see xes_data_writer.cpp).

\section server_mode Server Mode

For running with BEC, tpx3app supports the --server-mode commandline switch. The state diagram with most important actions is given below. Rest calls to the
ASI server in orange, actions by BEC in blue, and actions by the ASI server in green.

\image html program_states.png width=80%

In server mode, tpx3app can be controlled via REST API calls. The control interface address is given with the --control switch. A description of the
REST API calls can be produced with the "restcalls" compile target:

\code{.unparsed}
$ ./compile.sh restcalls
\endcode

The current program state is pushed on a websocket, available at /ws of the control interface.

When the program is not running in server mode, the configuration is received via files, and tpx3app sends /measurement/start to the ASI server.

\section issues_sec Issues

- The event packets need to come in order currently. Maybe this is too restrictive.
- The priority queue size determines the maximum tolerance to event reordering in the event stream. The default size might not be approprioate.
- Missing requirements for many aspects, like logging, exception handling, and configuration.
- Missing requirements for the software environment the analysis process will be embedded into.
- No thorough testing has been done

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
  and TCP adresses.

Assuming you have input files that make sense:

\code{.unparsed}
$ ./tpx3app --loglevel=warning
\endcode

And hopefully you'll have some output in the folder specified via the Processing.inp file.

\section docu Documentation

This documentation has been produced by

\code{.unparsed}
$ ./compile.sh doc
\endcode

This produces browsable html documentation at doc/html/index.html. The doc target requires doxygen in a version compatible to the
config file at doc/doxygen.cfg

Help on the compile targets can be produced by

\code{.unparsed}
$ ./compile.sh help
\endcode

Help on the commandline switches for programs can be produced by giving the --help switch to the program. Version information is
available for the tpx3app with the --version switch.

\code{.unparsed}
$ ./tpx3app --help
$ ./tpx3app --version
\endcode

Version information consists of branch-commit-date.

*/
