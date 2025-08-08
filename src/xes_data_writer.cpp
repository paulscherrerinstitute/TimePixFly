/*!
\file
Provide XES data writer implementations
*/

#include <cstdint>

#include "Poco/Exception.h"
#include "Poco/URI.h"
#include "Poco/Net/SocketAddress.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/SocketStream.h"
#include "Poco/JSON/PrintHandler.h"

#include "global.h"
#include "xes_data_writer.h"

namespace {

    /*!
    \brief Write XES data to file
    */
    class FileWriter : public xes::Writer {
        std::string basePath;   //!< Base file path (withouth -{period}.xes)

    public:
        /*!
        \brief Constructor
        \param path Base file path (withouth -{period}.xes)
        */
        inline FileWriter(const std::string& path)
            : basePath{path}
        {}

        inline ~FileWriter() = default; //!< Destructor

        /*!
        \brief Write XES data to file
        The data is written to file {basePath}-{period}.xes
        \param data XES Data
        \param period Which period
        */
        inline void write(const xes::Data& data, period_type period) override
        {
            std::ofstream OutFile(basePath + "-" + std::to_string(period) + ".xes");

            const auto& TDSpectra = data.TDSpectra;
            const auto NumEnergyPoints = data.detector->energy_points.npoints;
            const auto TRoiN = data.detector->TRoiN;
            for (std::remove_cv_t<decltype(NumEnergyPoints)> i=0; i<NumEnergyPoints; i++) {
                    for (std::remove_cv_t<decltype(TRoiN)> j=0; j<TRoiN; j++) {
                            OutFile << TDSpectra[j * NumEnergyPoints + i] << " ";
                    }
                    OutFile << "\n";
            }
            if (OutFile.fail())
                    throw std::ios_base::failure("xes::FileWriter::write failed");
            OutFile.close();
        }

        inline std::string dest() override
        {
            return basePath;
        }
    };

    /*!
    \brief Write XES data to TCP address
    */
    class TcpWriter : public xes::Writer {
        Poco::Net::StreamSocket dataReceiver;   //!< Connected socket to receiver

    public:
        /*!
        \brief Constructor
        \param address Hostname and port in the form {host}:{port}
        */
        inline TcpWriter(const std::string& address)
        {
            try {
                dataReceiver.connect(Poco::Net::SocketAddress{address});
            } catch (Poco::Exception& ex) {
                throw Poco::RuntimeException(std::string{"Connection to "} + address + " failed: " + ex.displayText());
            }
        }

        inline ~TcpWriter() = default;  //!< Destructor

        /*!
        \brief Write XES data to TCP address
        Send: { "type":"XesData", "Period":{period}, "TDSpectra":[{ep0}, {ep1}, ..., {epNxM}] }
        \param data XES Data
        \param period Which period
        */
        inline void write(const xes::Data& data, period_type period) override
        {
            const auto& TDSpectra = data.TDSpectra;
            const auto elements = TDSpectra.size();
            Poco::Net::SocketStream send{dataReceiver};

            send << R"({"type":"XesData","period":)" << period
                 << R"(,"TDSpectra":[)" << TDSpectra[0];
            for (std::remove_cv_t<decltype(elements)> i=1; i<elements; i++)
                send << ',' << TDSpectra[i];
            send << R"(],"totalEvents":)" << data.Total
                 << R"(,"beforeROI":)" << data.BeforeRoi
                 << R"(,"afterROI":)" << data.AfterRoi
                 << "}\n" << std::flush;
        }

        inline void start(const Detector& detector) override
        {
            Poco::Net::SocketStream send{dataReceiver};
            {
                Poco::JSON::PrintHandler json{send};
                json.startObject();
                json.key("type"); json.value(std::string{"StartFrame"});
                json.key("Mode"); json.value(std::string{detector.TOAMode ? "TOA" : "TOT"});
                json.key("TRoiStart"); json.value(detector.TRoiStart);
                json.key("TRoiStep"); json.value(detector.TRoiStep);
                json.key("TRoiN"); json.value(detector.TRoiN);
                json.key("NumEnergyPoints"); json.value(detector.energy_points.npoints);
                json.key("save_interval"); json.value(global::instance->save_interval);
                json.endObject();
            }
            send << '\n' << std::flush;
        }

        inline void stop(const std::string& error_message) override
        {
            Poco::Net::SocketStream send{dataReceiver};
            {
                Poco::JSON::PrintHandler json{send};
                json.startObject();
                json.key("type"); json.value(std::string{"EndFrame"});
                json.key("error"); json.value(error_message.empty() ? std::string{global::no_error} : error_message);
                json.endObject();
            }
            send << '\n' << std::flush;
        }

        inline std::string dest() override
        {
            return dataReceiver.peerAddress().toString();
        }
    };

}

namespace xes {

    Writer::~Writer()
    {}

    void Writer::start([[maybe_unused]] const Detector& detector)
    {}

    void Writer::stop([[maybe_unused]] const std::string& error_message)
    {}

    std::unique_ptr<Writer> Writer::from_uri(const std::string& uri)
    {
        Poco::URI destination{uri};
        const std::string& scheme{destination.getScheme()};
        const std::string dest{destination.getPathEtc()};
        if (scheme == "file") {
            return std::unique_ptr<Writer>{new FileWriter{dest}};
        } else if (scheme == "tcp") {
            return std::unique_ptr<Writer>{new TcpWriter{dest}};
        } else
            throw Poco::UnknownURISchemeException{scheme + " - unsupported uri scheme"};
        return nullptr;
    }

}
