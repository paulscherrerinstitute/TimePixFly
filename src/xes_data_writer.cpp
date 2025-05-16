/*!
\file
Provide XES data writer implementations
*/

#include <cstdint>
#include <cassert>

#include "Poco/Exception.h"
#include "Poco/URI.h"
#include "Poco/Net/SocketAddress.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/SocketStream.h"

#include "pixel_index.h"
#include "energy_points.h"
#include "xes_data_writer.h"

namespace {

    /*!
    \brief Write XES data to file
    */
    class FileWriter : public xes::Writer {
        std::string basePath;   //!< Base file path (withouth -<period>.xes)

    public:
        /*!
        \brief Constructor
        \param path Base file path (withouth -<period>.xes)
        */
        inline FileWriter(const std::string& path)
            : basePath{path}
        {}

        inline ~FileWriter() = default; //!< Destructor

        /*!
        \brief Write XES data to file
        The data is written to file <basePath>-<period>.xes
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
    };

    /*!
    \brief Write XES data to TCP address
    */
    class TcpWriter : public xes::Writer {
        Poco::Net::StreamSocket dataReceiver;   //!< Connected socket to receiver

    public:
        /*!
        \brief Constructor
        \param address Hostname and port in the form <host>:<port>
        */
        inline TcpWriter(const std::string& address)
            : dataReceiver{Poco::Net::SocketAddress{address}}
        {}

        inline ~TcpWriter() = default;  //!< Destructor

        /*!
        \brief Write XES data to TCP address
        Send: { "Period":<period>, "TDSpectra":[<ep0>, <ep1>, ..., <epNxM>] }
        \param data XES Data
        \param period Which period
        */
        inline void write(const xes::Data& data, period_type period) override
        {
            const auto& TDSpectra = data.TDSpectra;
            const auto elements = TDSpectra.size();
            Poco::Net::SocketStream send{dataReceiver};

            send << "{\"Period\":" << period
                 << ",\"TDSpectra\":[" << TDSpectra[0];
            for (std::remove_cv_t<decltype(elements)> i=1; i<elements; i++)
                send << ',' << TDSpectra[i];
            send << "]}" << std::flush;
        }
    };

}

namespace xes {

    Writer::~Writer()
    {}

    std::unique_ptr<Writer> Writer::from_uri(const std::string& uri)
    {
        Poco::URI destination{uri};
        const std::string& scheme{destination.getScheme()};
        const std::string dest{destination.getPathEtc()};
        if (scheme == "file")
            return std::unique_ptr<Writer>{new FileWriter{dest}};
        else if (scheme == "tcp")
            return std::unique_ptr<Writer>{new TcpWriter{dest}};
        else
            throw Poco::UnknownURISchemeException{scheme + " - unsupported uri scheme"};
        return nullptr;
    }

}
