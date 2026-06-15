#pragma once

#ifndef ASI_SERVER_H
#define ASI_SERVER_H

/*!
\file
Provide communication code to the serval ASI detector server
*/

#include <Poco/URI.h>
#include "Poco/Net/MediaType.h"
#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/HTTPResponse.h"
#include "Poco/JSON/Parser.h"

#include "global.h"
#include "json_ops.h"

/*!
\brief ASI detector server communication functions
*/
namespace asi {

    using Poco::Net::SocketAddress;
    using Poco::URI;
    using Poco::Net::MediaType;
    using Poco::Net::HTTPClientSession;
    using Poco::Net::HTTPRequest;
    using Poco::Net::HTTPResponse;

    /*!
    \brief ASI detector server connection and functions
    */
    class server final {

        Logger& logger;                                     //!< Poco::Logger object
        std::unique_ptr<HTTPClientSession> clientSession;   //!< Client session with ASI server

      public:
        unsigned num_chips = 0u;    //!< Number of TPX3 chips on detector, set by read_info()

      private:
        /*!
        \brief Map request string to Uri
        \param requestString HTTP request string
        \return URI string
        */
        inline std::string getUri(const std::string& requestString) const
        {
            logger << "getUri(" << requestString << ')' << log_trace;
            // std::ostringstream oss;
            // oss << "http:" << requestString;
            // return URI{oss.str()}.toString();
            return URI{requestString}.toString();
        }

        /*!
        \brief HTTP GET request to ASI server
        \param requestString    HTTP request string
        \param response         Poco HTTP response object reference
        \return Input stream reference for reading HTTP GET response content
        */
        inline std::istream& serverGet(const std::string& requestString, HTTPResponse& response) const
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
        inline std::ostream& serverPut(const std::string& requestString, const std::string& contentType, std::streamsize contentLength) const
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
        \brief Check HTTP response
        \param response Poco HTTP response reference
        \param in       Poco HTTP response input stream reference
        \throw RuntimeException with error response if the response status is not OK
        */
        inline void checkResponse(const HTTPResponse& response, std::istream& in) const
        {
            if (response.getStatus() != HTTPResponse::HTTP_OK) {
                std::ostringstream oss;
                oss << "request failed (" << response.getStatus() << "): " << response.getReason() << '\n' << in.rdbuf();
                throw Poco::RuntimeException(oss.str(), __LINE__);
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
            Poco::JSON::Parser jsonParser;
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
        inline std::istream& putJsonString(const std::string& requestString, const std::string& jsonString, HTTPResponse& response) const
        {
            logger << "putJsonString(" << requestString << ", " << jsonString << ")" << log_trace;
            auto& out = serverPut(requestString, "application/json", jsonString.size());
            out << jsonString;
            return clientSession->receiveResponse(response);
        }

        // /*!
        // \brief HTTP PUT request with JSON object argument
        // \param requestString    HTTP request string
        // \param objPtr           Poco JSON object pointer
        // \param response         Poco HTTP response object reference
        // \return Input stream reference for reading response
        // */
        // inline std::istream& putJsonObject(const std::string& requestString, Poco::JSON::Object::Ptr objPtr, HTTPResponse& response) const
        // {
        //     logger << "putJsonObject(" << requestString << ")" << log_trace;
        //     std::ostringstream oss;
        //     objPtr->stringify(oss);
        //     return putJsonString(requestString, oss.str(), response);
        // }

        /*!
        \brief Get detector layout JSON object from ASI server
        \return Poco pointer to detector layout JSON object
        */
        inline Poco::JSON::Object::Ptr detectorLayout()
        {
            logger << "detectorLayout()" << log_trace;
            return getJsonObject("/detector/layout");
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

      public:
        /*!
        \brief Constructor
        \param log Logger reference
        */
        inline explicit server(Logger& log)
            : logger(log)
        {}

        /*!
        \brief Connect to ASI server
        */
        inline void connect()
        {
            const auto& gvars = *global::instance;
            logger << "connecting to ASI server at " << gvars.serverAddress.toString() << log_notice;
            clientSession.reset(new HTTPClientSession{gvars.serverAddress});
        }

        /*!
        \brief Detector initialization request to ASI server
        \param bpcFilePath BPC config file path
        \param dacsFilePath DACS config file path
        */
        inline void detector_init(const std::string& bpcFilePath, const std::string& dacsFilePath)
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
        \brief Send raw event stream destination IP and port information to ASI server
        \param address TCP address
        */
        inline void configure_raw_destination(const SocketAddress& address)
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
        inline void acquisition_start()
        {
            logger << "acquisitionStart()" << log_trace;
            HTTPResponse response;
            auto& in = serverGet("/measurement/start", response);
            checkResponse(response, in);
            logger << "Response of acquisition start: " << in.rdbuf() << log_notice;
            checkSession(in);
        }

        /*!
        \brief Log the serval dashboard with the serval version
        */
        inline void log_dashboard()
        {
            auto dashboardPtr = dashboard();
            std::string softwareVersion = (dashboardPtr|"Server")->getValue<std::string>("SoftwareVersion");
            LogProxy log(logger);
            log << "Server Software Version: " << softwareVersion << "\nDashboard: ";
            dashboardPtr->stringify(log.base());
            log << log_notice;
        }

        /*!
        \brief Log the detector configuration
        */
        inline void log_config()
        {
            auto configPtr = detectorConfig();
            LogProxy log(logger);
            log << "Response of getting the Detector Configuration from SERVAL: ";
            configPtr->stringify(log.base());
            log << log_notice;
        }

        /*!
        \brief Read and log detector info
        */
        inline void read_info()
        {
            auto infoPtr = detectorInfo();
            {
                LogProxy log(logger);
                log << "Response of getting the Detector Info from SERVAL: ";
                infoPtr->stringify(log.base());
                log << log_notice;
            }

            infoPtr->get("NumberOfChips").convert(num_chips);
        }

        /*!
        \brief Read and log detector layout
        \param layout Read layout into this variable
        */
        inline void read_layout(detector_layout& layout)
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
            for (unsigned i=0; i<num_chips; i++) {
                chip_position chip;
                (chipPtr | i)->get("X").convert(chip.x);
                (chipPtr | i)->get("Y").convert(chip.y);
                layout.chip.push_back(chip);
            }

            LogProxy log(logger);
            log << layout << log_debug;
        }
    };

}

#endif
