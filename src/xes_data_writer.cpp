/*!
\file
Provide XES data writer implementations
*/

#include "Poco/Exception.h"
#include "Poco/URI.h"
#include "Poco/Net/SocketAddress.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/SocketStream.h"
#include "Poco/JSON/PrintHandler.h"
#include "Poco/Redis/Client.h"
#include "Poco/Redis/Command.h"

#include "xes_data_writer.h"

namespace {

    /*!
    \brief Write XES data to file
    */
    class FileWriter final : public xes::Writer {
        std::string basePath;   //!< Base file path (withouth -{period}.xes)

    public:
        /*!
        \brief Constructor
        \param path Base file path (withouth -{period}.xes)
        */
        inline FileWriter(const std::string& path)
            : basePath{path}
        {}

        inline ~FileWriter() override = default; //!< Destructor

        /*!
        \brief Write XES data to file
        The data is written to file {basePath}-{period}.xes
        \param data XES Data
        */
        inline void write(const xes::Data& data) override
        {
            const std::string path{basePath + "-" + std::to_string(data.period) + ".xes"};
            std::ofstream OutFile(path);
            if (! OutFile.good())
                throw std::ios_base::failure("xes::FileWriter::write unable to open file - " + path);

            const auto& gvars = *global::instance;
            const auto& TDSpectra = data.TDSpectra;
            const auto NumEnergyPoints = gvars.pix_map->npoints;
            const auto TRoiN = gvars.time_roi.TRoiN;
            for (std::remove_cv_t<decltype(NumEnergyPoints)> i=0; i<NumEnergyPoints; i++) {
                for (std::remove_cv_t<decltype(TRoiN)> j=0; j<TRoiN; j++) {
                        OutFile << TDSpectra[j * NumEnergyPoints + i] << " ";
                }
                OutFile << "\n";
            }
            if (OutFile.fail())
                throw std::ios_base::failure("xes::FileWriter::write failed");
            OutFile.close();
            data_counter++;
        }

        /*!
        \brief Destination file uri
        \return `file:<file path>`
        */
        inline std::string dest() override
        {
            return std::string{"file:"} + basePath;
        }
    };

    /*!
    \brief Write XES data to TCP address
    */
    class TcpWriter final : public xes::Writer {
        Poco::Net::StreamSocket dataReceiver;   //!< Connected socket to receiver

    public:
        /*!
        \brief Constructor
        Connects to TCP address
        \param address Hostname and port in the form {host}:{port}
        */
        inline TcpWriter(const std::string& address)
        {
            try {
                dataReceiver.connect(Poco::Net::SocketAddress{address});
            } catch (Poco::Exception& ex) {
                throw Poco::RuntimeException(std::string{"Connection to output address <"} + address + "> failed: " + ex.displayText());
            }
        }

        /*!
        \brief Destructor
        Closes the connection to TCP address
        */
        inline ~TcpWriter() override
        {
            try {
                dataReceiver.close();
            } catch(...) {}
        }

        /*!
        \brief Write XES data to TCP address
        DATA PACKET:
        {
            "type":"XesData",
            "period":{period},
            "TDSpectra":[{ep0}, {ep1}, ..., {epNxM}],
            "totalEvents":{totalEvents},
            "beforeROI":{BeforeRoi},
            "afterROI":{AfterRoi}
        }
        when: for every save_interval after start
        \param data XES Data
        */
        inline void write(const xes::Data& data) override
        {
            const auto& TDSpectra = data.TDSpectra;
            const auto elements = TDSpectra.size();
            Poco::Net::SocketStream send{dataReceiver};

            send << R"({"type":"XesData","period":)" << data.period
                 << R"(,"TDSpectra":[)" << TDSpectra[0];
            for (std::remove_cv_t<decltype(elements)> i=1; i<elements; i++)
                send << ',' << TDSpectra[i];
            send << R"(],"totalEvents":)" << data.Total
                 << R"(,"beforeROI":)" << data.BeforeRoi
                 << R"(,"afterROI":)" << data.AfterRoi
                 << "}\n" << std::flush;
            data_counter++;
            if (data.period > last_period)
                last_period = data.period;
        }

        /*!
        \brief Send XES start frame to TCP address
        DATA PACKET:
        {
            "type":"StartFrame",
            "Mode":"TOA",
            "TRoiStart":{TRoiStart},
            "TRoiStep":{TRoiStep},
            "TRoiN":{TRoiN},
            "NumEnergyPoints":{npoints},
            "save_interval":{save_interval}
        }
        when: at start of measurement
        Resets \ref last_period
        \param time_roi Time ROI
        */
        inline void start(const TimeRoi& time_roi) override
        {
            Poco::Net::SocketStream send{dataReceiver};
            {
                Poco::JSON::PrintHandler json{send};
                json.startObject();
                json.key("type"); json.value(std::string{"StartFrame"});
                json.key("Mode"); json.value(std::string{"TOA"});
                json.key("TRoiStart"); json.value(time_roi.TRoiStart);
                json.key("TRoiStep"); json.value(time_roi.TRoiStep);
                json.key("TRoiN"); json.value(time_roi.TRoiN);
                json.key("NumEnergyPoints"); json.value(global::instance->pix_map->npoints);
                json.key("save_interval"); json.value(global::instance->save_interval);
                json.endObject();
            }
            send << '\n' << std::flush;
            last_period = 0u;
        }

        /*!
        \brief Send XES end frame to TCP address
        \param error_message Error message, will be set to "none" if empty
        DATA PACKET:
        {
            "type":"EndFrame",
            "periods":131000,   // last_period
            "error":"error_message"
        }
        when: at end of measurement
        Resets \ref last_period
        */
        inline void stop(const std::string& error_message) override
        {
            Poco::Net::SocketStream send{dataReceiver};
            {
                Poco::JSON::PrintHandler json{send};
                json.startObject();
                json.key("type"); json.value(std::string{"EndFrame"});
                json.key("periods"); json.value(last_period);
                json.key("error"); json.value(error_message.empty() ? std::string{global::no_error} : error_message);
                json.endObject();
            }
            send << '\n' << std::flush;
            last_period = 0u;
        }

        /*!
        \brief Destination TCP uri
        \return `tcp://<host>:<port>`
        */
        inline std::string dest() override
        {
            return std::string{"tcp://"} + dataReceiver.peerAddress().toString();
        }
    };

    /*!
    \brief Publish XES data to REDIS channel
    */
    class RedisWriter final : public xes::Writer {

        /*!
        \brief Cached connection object
        */
        class RedisPublisher final {
            Poco::Redis::Client redis_client;   //!< Client connection
            std::string host_port;              //!< host:port for comparison
            std::string channel;                //!< REDIS channel for publishing
            std::string scan;                   //!< Scan ID to use in metadata

          public:
            /*!
            \brief Constructor

            Creates the connection
            \param address `host:port`
            */
            inline RedisPublisher(const std::string& address)
                : redis_client(address), host_port{address}
            {}

            /*!
            \brief Destructor

            Disconnects from REDIS
            */
            inline ~RedisPublisher()
            {
                redis_client.disconnect();
            }

            /*!
            \brief Publish XES data to REDIS channel
            DATA PACKET:
            {
                "type":"XesData",
                "period":{period},
                "TDSpectra":[{ep0}, {ep1}, ..., {epNxM}],
                "totalEvents":{totalEvents},
                "beforeROI":{BeforeRoi},
                "afterROI":{AfterRoi},
                "scanID":"{scan}"
            }
            when: for every save_interval after start
            \param data XES Data
            */
            inline void write(const xes::Data& data) const
            {
                std::ostringstream oss;
                {
                    const auto& TDSpectra = data.TDSpectra;
                    const auto elements = TDSpectra.size();

                    oss << R"({"type":"XesData","period":)" << data.period
                        << R"(,"TDSpectra":[)" << TDSpectra[0];
                    for (std::remove_cv_t<decltype(elements)> i=1; i<elements; i++)
                        oss << ',' << TDSpectra[i];
                    oss << R"(],"totalEvents":)" << data.Total
                        << R"(,"beforeROI":)" << data.BeforeRoi
                        << R"(,"afterROI":)" << data.AfterRoi
                        << R"(,"scanID":")" << scan
                        << R"("})";
                }

                Poco::Redis::Command cmd("PUBLISH");
                cmd << channel << oss.str();
            }

            /*!
            \brief Publish XES start frame to REDIS channel
            DATA PACKET:
            {
                "type":"StartFrame",
                "Mode":"TOA",
                "TRoiStart":{TRoiStart},
                "TRoiStep":{TRoiStep},
                "TRoiN":{TRoiN},
                "NumEnergyPoints":{npoints},
                "save_interval":{save_interval},
                "scanID":"{scan}"
            }
            when: at start of measurement
            Resets \ref last_period
            \param time_roi Time ROI
            */
            inline void start(const TimeRoi& time_roi) const
            {
                std::ostringstream oss;
                {
                    Poco::JSON::PrintHandler json{oss};
                    json.startObject();
                    json.key("type"); json.value(std::string{"StartFrame"});
                    json.key("Mode"); json.value(std::string{"TOA"});
                    json.key("TRoiStart"); json.value(time_roi.TRoiStart);
                    json.key("TRoiStep"); json.value(time_roi.TRoiStep);
                    json.key("TRoiN"); json.value(time_roi.TRoiN);
                    json.key("NumEnergyPoints"); json.value(global::instance->pix_map->npoints);
                    json.key("save_interval"); json.value(global::instance->save_interval);
                    json.key("scanID"); json.value(scan);
                    json.endObject();
                }

                Poco::Redis::Command cmd("PUBLISH");
                cmd << channel << oss.str();
            }

            /*!
            \brief Publish XES end frame to REDIS channel
            \param error_message Error message, will be set to "none" if empty
            \param last_period Last recorded period
            DATA PACKET:
            {
                "type":"EndFrame",
                "periods":131000,   // last_period
                "error":"error_message",
                "scanID":"{scan}"
            }
            when: at end of measurement
            Resets \ref last_period
            */
            inline void stop(const std::string& error_message, period_type last_period) const
            {
                std::ostringstream oss;
                {
                    Poco::JSON::PrintHandler json{oss};
                    json.startObject();
                    json.key("type"); json.value(std::string{"EndFrame"});
                    json.key("periods"); json.value(last_period);
                    json.key("error"); json.value(error_message.empty() ? std::string{global::no_error} : error_message);
                    json.key("scanID"); json.value(scan);
                    json.endObject();
                }

                Poco::Redis::Command cmd("PUBLISH");
                cmd << channel << oss.str();
            }

            /*!
            \brief REDIS destination uri
            \return `redis://<host>:<port>/<key>?scan-id=<scan>`
            */
            inline std::string dest() const
            {
                return std::string{"redis://"} + redis_client.address().toString() + '/' + channel + "?scan-id=" + scan;
            }

            /*!
            \brief Set parameters that do not affect the connection
            \param channel_key REDIS key
            \param scan_id Scan number
            */
            inline void setParam(const std::string& channel_key, const std::string& scan_id) noexcept
            {
                channel = channel_key;
                scan = scan_id;
            }

            /*!
            \brief Compare connection related parameters
            \param host_port String "host:port"
            \return True if this connection matches the connection parameters
            */
            inline bool hasAddress(const std::string& host_port) const noexcept
            {
                return host_port == this->host_port;
            }
        };

        inline static std::unique_ptr<RedisPublisher> publisherCache{nullptr};  //!< Caches REDIS connection object

      public:
        /*!
        \brief Constructor

        Will only create the cached connection object if the connection parameters do not match
        \param address REDIS connection uri `redis://host:port/key?scan-id=xxxx`
        */
        inline RedisWriter(const Poco::URI& address)
        {
            const std::string authority{address.getAuthority()};         // host:port
            const std::string key{address.getPath().substr(1)};     // remove leading char in /key
            const std::string scan{address.getQuery().substr(8)};   // remove up to = in scan-id=xxxx

            if ((publisherCache == nullptr) ||
                !publisherCache->hasAddress(authority))
            {
                publisherCache.reset(new RedisPublisher{authority});
            }

            publisherCache->setParam(key, scan);
        }

        /*!
        \brief Publish XES data frame

        Updates `last_period` cache
        \see RedisPublisher::write()
        \param data XES data
        */
        inline void write(const xes::Data& data) override
        {
            assert(publisherCache != nullptr);
            publisherCache->write(data);
                
            data_counter++;
            if (data.period > last_period)
                last_period = data.period;
        }

        /*!
        \brief Publish XES start frame
        \see RedisPublisher::start()
        \param time_roi Time ROI
        */
        inline void start(const TimeRoi& time_roi) override
        {
            assert(publisherCache != nullptr);
            publisherCache->start(time_roi);
        }

        /*!
        \brief Publish XES end frame
        \see RedisPublisher::stop()
        \param error_message Error message, or ""
        */
        inline void stop(const std::string& error_message)  override
        {
            assert(publisherCache != nullptr);
            publisherCache->stop(error_message, last_period);
        }

        /*!
        \brief REDIS destination uri
        \see RedisPublisher::dest()
        \return Redis destination uri
        */
        inline std::string dest() override
        {
            assert(publisherCache != nullptr);
            return publisherCache->dest();
        }        
    };
}

namespace xes {

    Writer::~Writer()
    {}

    void Writer::start([[maybe_unused]] const TimeRoi& time_roi)
    {}

    void Writer::stop([[maybe_unused]] const std::string& error_message)
    {}

    std::unique_ptr<Writer> Writer::from_uri(const std::string& uri)
    {
        Poco::URI destination{uri};
        const std::string& scheme{destination.getScheme()};
        if (scheme == "redis") {
            return std::unique_ptr<Writer>{new RedisWriter{destination}};
        } else if (scheme == "file") {
            return std::unique_ptr<Writer>{new FileWriter{destination.getPathEtc()}};
        } else if (scheme == "tcp") {
            return std::unique_ptr<Writer>{new TcpWriter{destination.getPathEtc()}}; // TODO: change to getAuthority
        } else
            throw Poco::UnknownURISchemeException{std::string{"bad output uri - <"} + scheme + "> is an unsupported uri scheme, use file:filename or tcp:host:port"};
        return nullptr;
    }

}
