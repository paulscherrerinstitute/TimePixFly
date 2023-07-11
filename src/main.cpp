// Implement raw-stream-.example.py in C++

// Author: hans-christian.stadler@psi.ch

// #include <filesystem>
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
// #include <string_view>
#include <thread>
#include <mutex>
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
#include "Poco/Logger.h"
#include "Poco/Message.h"
#include "Poco/ByteOrder.h"
#include "Poco/Process.h"

#include "decoder.h"
#include "io_buffers.h"

using Poco::Logger;
using wall_clock = std::chrono::high_resolution_clock;

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
    using Poco::Message;
    using Poco::LogicException;
    using Poco::InvalidArgumentException;
    using Poco::RuntimeException;
    using Poco::ReadFileException;
    using Poco::DataFormatException;
    using Poco::Dynamic::Var;

    // A fatal error. The application will most likely terminate. This is the highest priority.
    [[maybe_unused]] constexpr Message::Priority log_fatal = Message::PRIO_FATAL;

    // A critical error. The application might not be able to continue running successfully.
    [[maybe_unused]] constexpr Message::Priority log_critical = Message::PRIO_CRITICAL;

    // An error. An operation did not complete successfully, but the application as a whole is not affected.
    [[maybe_unused]] constexpr Message::Priority log_error = Message::PRIO_ERROR;

    // A warning. An operation completed with an unexpected result.
    [[maybe_unused]] constexpr Message::Priority log_warn = Message::PRIO_WARNING;

    // A notice, which is an information with just a higher priority.
    [[maybe_unused]] constexpr Message::Priority log_notice = Message::PRIO_NOTICE;

    // An informational message, usually denoting the successful completion of an operation.
    [[maybe_unused]] constexpr Message::Priority log_info = Message::PRIO_INFORMATION;

    // A debugging message.
    [[maybe_unused]] constexpr Message::Priority log_debug = Message::PRIO_DEBUG;

    // A tracing message. This is the lowest priority.
    constexpr Message::Priority log_trace = Message::PRIO_TRACE;

    struct LogProxy final : public std::ostringstream {
        Logger& logger;

        inline LogProxy(Logger& l)
            : logger(l)
        {}

        inline LogProxy(LogProxy&& other)
            : logger(other.logger)
        {}

        LogProxy(const LogProxy&) = delete;
        LogProxy& operator=(const LogProxy&) = delete;
        LogProxy& operator=(LogProxy&&) = delete;

        inline virtual ~LogProxy() {}
    };

    template<typename T>
    inline LogProxy operator<< (Logger& logger, const T& value)
    {
        LogProxy proxy(logger);
        proxy << value;
        return proxy;
    }

    template<typename T>
    inline LogProxy& operator<< (LogProxy& proxy, const T& value)
    {
        static_cast<std::ostringstream&>(proxy) << value;
        return proxy;
    }

    inline LogProxy& operator<< (LogProxy& proxy, const Message::Priority& priority)
    {
        proxy.logger.log(Message("Tpx3App", proxy.str(), priority));
        proxy.str("");
        return proxy;
    }

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

    template<typename Decode>
    class DataHandler final {
        //      tpxHeader = int.from_bytes(b'TPX3', 'little')
        // const uint32_t tpxHeader = Poco::ByteOrder::fromLittleEndian(*reinterpret_cast<const uint64_t*>(tpxBytes.data()));
        constexpr static uint64_t tpxHeader = 861425748UL; // 'TPX3' as uint64_t
        #if SERVER_VERSION >= 320
            uint64_t DATA_OFFSET = 8;   // start of event data
        #else
            uint64_t DATA_OFFSET = 0;   // start of event data
        #endif

        StreamSocket& dataStream;
        Logger& logger;
        io_buffer_pool_collection perChipBufferPool;
        const size_t bufferSize;
        std::thread readerThread;
        std::vector<std::thread> analyserThreads;
        std::mutex coutMutex;
        std::mutex memberMutex;
        std::atomic<unsigned> analyzerReady = 0;
        std::atomic<bool> stopOperation = false;

        bool stop() const
        {
            return stopOperation.load(std::memory_order_relaxed);
        }

        void stopNow()
        {
            stopOperation.store(true, std::memory_order_relaxed);
        }

        int readData(void* buf, int size)
        {
            logger << "readData(" << buf << ", " << size << ')' << log_trace;
            int numBytes = 0;

            do {
                int numRead = dataStream.receiveBytes(&static_cast<char*>(buf)[numBytes], size - numBytes);
                if (numRead == 0)
                    break;
                numBytes += numRead;
            } while (numBytes < size);

            return numBytes;
        }

        int readPacketHeader(uint64_t& chipIndex, uint64_t& chunkSize, uint64_t& packetId)
        {
            logger << "readPacketHeader()" << log_trace;
            #if SERVER_VERSION >= 320
                uint64_t header[2];
            #else
                uint64_t header[1];
            #endif
            
            int numRead = readData(header, sizeof(header));
            if (numRead == 0)
                return 0;
            if (numRead != sizeof(header))
                throw ReadFileException(std::string("unable to read packet header") + std::to_string(numRead));

            logger << "packed header: " << std::hex << header[0]
                #if SERVER_VERSION >= 320
                   << ' ' << header[1]
                #endif
                   << std::dec << log_debug;

            //         if isChip:
            //             chipIndex = get_bits(d, 39, 32)
            //             # print('ChipIndex: ', chipIndex)
            if ((header[0] & 0xffffffffUL) != tpxHeader)
                throw DataFormatException("chunk header expected");
            chipIndex = Decode::getBits(header[0], 39, 32);
            chunkSize = Decode::getBits(header[0], 63, 48);
            #if SERVER_VERSION >= 320
                if (!Decode::matchesByte(header[1], 0x50))
                    throw DataFormatException("packet id expected");
                packetId = Decode::getBits(header[1], 47, 0);
                logger << "packet header: chipIndex " << chipIndex << ", chunSize " << chunkSize << ", packetId " << packetId << log_info;
            #else
                packetId = 0;
                logger << "packet header: chipIndex " << chipIndex << ", chunSize " << chunkSize << log_info;
            #endif

            return numRead;
        }

        void readData()
        {
            // with connection:
            //     while reading:

            //         # Read a multiple of 8 into buffer_
            //         read = 0
            //         while read == 0 or read % 8 != 0:
            //             newly_read = connection.recv_into(view[read:], bytes_to_read - read)

            //             if newly_read == 0:
            //                 reading = False
            //                 break

            //             read += newly_read
            double spinTime = .0;
            double workTime = .0;

            try {
                do {
                    uint64_t chipIndex = 0;
                    uint64_t chunkSize = 0;
                    uint64_t packetId = 0;
                    uint64_t totalBytes = DATA_OFFSET;
                    int bytesRead;

                    {
                        const auto t1 = wall_clock::now();
                        bytesRead = readPacketHeader(chipIndex, chunkSize, packetId);
                        const auto t2 = wall_clock::now();
                        workTime += std::chrono::duration<double>(t2 - t1).count();
                        if (bytesRead == 0)
                            break;
                    }

                    while (totalBytes < chunkSize) {
                        auto& bufferPool = *perChipBufferPool[chipIndex];

                        const auto t1 = wall_clock::now();

                        auto* eventBuffer = bufferPool.get_empty_buffer();
                        if (eventBuffer == nullptr)
                            throw LogicException("received nullptr as empty buffer");

                        const auto t2 = wall_clock::now();

                        char* data = eventBuffer->content.data();
                        eventBuffer->content_offset = totalBytes;
                        eventBuffer->chunk_size = chunkSize;

                        do {
                            const int bytesBuffered = eventBuffer->content_size;
                            const int restCapacity = bufferSize - bytesBuffered;
                            const int restData = chunkSize - totalBytes;
                            const int readSize = std::min(restCapacity, restData);
                            bytesRead = dataStream.receiveBytes(&data[bytesBuffered], readSize);
                            totalBytes += bytesRead;

                            logger << "read " << bytesRead << " bytes into buffer " << data << ", " << totalBytes << " total" << log_debug;

                            eventBuffer->content_size += bytesRead;

                            if (bytesRead <= 0)
                                throw ReadFileException("no bytes received");
                            if (bytesRead == readSize)
                                break;
                            if (stop())
                                goto reader_stopped;
                        } while (true);

                        const auto t3 = wall_clock::now();

                        bufferPool.put_nonempty_buffer({ packetId, std::move(eventBuffer) });

                        spinTime += std::chrono::duration<double>{t2 - t1}.count();
                        workTime += std::chrono::duration<double>{t3 - t2}.count();
                    }
                } while (true);
            } catch (Poco::Exception& ex) {
                stopNow();
                logger << "reader exception: " << ex.displayText() << log_critical;
            } catch (std::exception& ex) {
                stopNow();
                logger << "reader exception: " << ex.what() << log_critical;
            }

        reader_stopped:
            for (auto& pool : perChipBufferPool)
                pool->finish_writing();

            {
                std::lock_guard lock{memberMutex};
                readTime += workTime;
                readSpinTime += spinTime;
            }

            logger << "reader stopped" << log_debug;

        }

        void analyseData(unsigned threadId)
        {
            // def process_raw_data(buffer_, N):                

            //     chipIndex = 0
            //     count = 0

            const unsigned chipIndex = threadId;

            // init buffer pool / io_buffer_pool::buffer_size set elsewhere
            perChipBufferPool[chipIndex].reset(new io_buffer_pool{});
            analyzerReady.fetch_add(1, std::memory_order_release);

            double spinTime = .0;
            double workTime = .0;
            uint64_t hits = 0;

            try {
           
                uint64_t packetNumber = 0;
                auto& bufferPool = *perChipBufferPool[chipIndex];

                do {
                    uint64_t chunkSize = 0;
                    uint64_t totalBytes = DATA_OFFSET;

                    do {

                        const auto t1 = wall_clock::now();

                        auto [packetNumber, eventBuffer] = bufferPool.get_nonempty_buffer();

                        const auto t2 = wall_clock::now();

                        if (eventBuffer == nullptr) // no more data
                            goto analyser_stopped;
                        
                        size_t dataSize = eventBuffer->content_size;
                        chunkSize = eventBuffer->chunk_size;
                        logger << threadId << ": full buffer, chunk " << chunkSize
                                           << " offset " << eventBuffer->content_offset
                                           << " size " << eventBuffer->content_size << log_debug;

                        size_t processingByte = 0;
                        const char* content = eventBuffer->content.data();

                        //     for i in range(N):
                        while (processingByte < dataSize) {
                            //         d = int(buffer_[i])
                            uint64_t d = *reinterpret_cast<const uint64_t*>(&content[processingByte]);

                            //         isHit = matches_nibble(d, 0xb)
                            //         isTDC = matches_nibble(d, 0x6)
                            //         isChip = (d & ((1 << 32) - 1) == tpxHeader)
                            // bool isHit = matchesNibble(d, 0xb);
                            // bool isTdc = matchesNibble(d, 0x6);
                            // bool isChip = (d & 0xffffffffUL) == tpxHeader;

                            if ((d & 0xffffffffUL) == tpxHeader) {
                                throw RuntimeException("encountered chunk header within chunk");
                            } else if (Decode::matchesByte(d, 0x50)) {
                                throw RuntimeException("encountered packet ID within chunk");
                            } else if (Decode::matchesNibble(d, 0xb)) {
                                //         if isHit:
                                //             # Note: calculating TOF is harder, since the order of the pixel data
                                //             # is not guaranteed. Therefore in worst case all pixel data must be sorted.
                                //             toa = clock_to_float(get_TOA_clock(d))
                                //             tot = clock_to_float(get_TOT_clock(d), clock=40e6)
                                //             x, y = calculate_XY(d)
                                //             count += 1
                                //             print('Hit: ', chipIndex, x, y, tot, toa)
                                const int64_t toaclk = Decode::getToaClock(d);
                                const uint64_t totclk = Decode::getTotClock(d);
                                float toa = Decode::clockToFloat(toaclk);
                                float tot = Decode::clockToFloat(totclk, 40e6);
                                std::pair<uint64_t, uint64_t> xy = Decode::calculateXY(d);
                                hits++;
                                logger << threadId << ": Hit: " << chipIndex << ' ' << xy.first << ' ' << xy.second << ' ' << tot << ' ' << toa << "  (" << totclk << ' ' << toaclk << ' ' << std::hex << d << std::dec << ')' << log_info;
                            } else if (Decode::matchesNibble(d, 0x6)) {
                                //         elif isTDC:
                                //             tdc = clock_to_float(get_TDC_clock(d))
                                //             # print('TDC: ', tdc)
                                float tdc = Decode::clockToFloat(Decode::getTdcClock(d));
                                logger << threadId << ": TDC: " << tdc << log_info;
                            } else {
                                logger << threadId << ": unknown " << std::hex << d << std::dec << log_info;
                            }

                            processingByte += sizeof(uint64_t);
                        }

                        bufferPool.put_empty_buffer(std::move(eventBuffer));

                        if (processingByte != dataSize)
                            throw LogicException("processingByte != dataSize");

                        totalBytes += dataSize;

                        const auto t3 = wall_clock::now();

                        spinTime += std::chrono::duration<double>(t2 - t1).count();
                        workTime += std::chrono::duration<double>(t3 - t2).count();

                    } while (totalBytes < chunkSize);

                } while(! stop());

            } catch (Poco::Exception& ex) {
                stopNow();
                logger << threadId << ": analyser exception: " << ex.displayText() << log_critical;
            } catch (std::exception& ex) {
                stopNow();
                logger << threadId << ": analyser exception: " << ex.what() << log_critical;
            }

        analyser_stopped:
            {
                std::lock_guard lock{memberMutex};
                hitCount += hits;
                analyseTime += workTime;
                analyseSpinTime += spinTime;
            }

            //         print('Processed %i hits' % processed)
            logger << threadId << ": Processed " << hits << " hits" << log_info;
        }

    public:
        DataHandler(StreamSocket& socket, Logger& log, unsigned long bufSize, unsigned long numChips)
            : dataStream{socket}, logger{log}, perChipBufferPool{numChips}, bufferSize{bufSize}
        {
            io_buffer_pool::buffer_size = bufSize;
            logger << "DataHandler(" << socket.address().toString() << ", " << bufSize << ", " << numChips << ')' << log_trace;
            analyserThreads.resize(numChips);
        }

        void run_async()
        {
            for (unsigned i=0; i<analyserThreads.size(); i++)
                analyserThreads[i] = std::thread([this, i]{this->analyseData(i);});
            while (analyzerReady.load(std::memory_order_consume) != analyserThreads.size())
                std::this_thread::yield();
            readerThread = std::thread([this]{this->readData();});
        }

        void await()
        {
            readerThread.join();
            for (auto& thread : analyserThreads)
                thread.join();
        }

        uint64_t hitCount = 0;
        double readSpinTime = .0;
        double readTime = .0;
        double analyseSpinTime = .0;
        double analyseTime = .0;
    };

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

            DataHandler<AsiRawStreamDecoder> dataHandler(dataStream, logger, bufferSize, numChips);
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
