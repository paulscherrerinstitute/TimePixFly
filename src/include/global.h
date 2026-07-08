#pragma once

#ifndef GLOBAL_H
#define GLOBAL_H

/*!
\file
Global configuration and control data
*/

#include <utility>
#include <string_view>
#include <variant>
#include <atomic>
#include <mutex>

#include "Poco/JSON/Object.h"
#include <Poco/Net/SocketAddress.h>

#include "energy_points.h"
#include "cpu_mask.h"
#include "time_roi.h"

using Poco::Net::SocketAddress;

/*!
\brief Global configuration and control data
*/
struct global final {
    // Constants
    static constexpr std::string_view APP_NAME = "Tpx3App";                //!< Name (e.g. for syslog)
    static constexpr std::string_view no_error{"none"};                    //!< json error string for no error
    static constexpr unsigned collect_timeout{300000};                     //!< 300ms receive timeout for detector data
    static constexpr period_type min_save_interval{6000};                  //!< Minimal save_interval in detector frequency unit
    static constexpr unsigned min_xes_data_pool_size{5};                   //!< Minimal XES data pool size per thread

    // Callbacks
    using key_type = std::string;                                          //!< key = path (for PUT and GET) or path?key (for GET with key)
    using put_callback = std::variant<                                     //!< PUT(path) JSON -> string
        std::function<std::string(Poco::JSON::Object::Ptr)>,               //!< (json-obj) -> string or
        std::function<std::string(std::istream&)>                          //!< (istream) -> string
    >;
    using get_callback = std::function<std::string(const std::string&)>;   //!< GET(path?key) value -> string
    std::map<key_type, put_callback> put_callbacks;                        //!< PUT callbacks
    std::map<key_type, get_callback> get_callbacks;                        //!< GET callbacks
    using stop_handler = std::function<void()>;                            //!< Stop somethinig gracefully
    std::vector<stop_handler> stop_handlers;                               //!< Called by REST /?stop

    // Accessible by REST interface
    // Command flags
    std::atomic_bool stop_collect{false};                               //!< Stop collecting data
    std::atomic_bool stop{false};                                       //!< Stop server
    std::atomic_bool restart{false};                                    //!< Restart server
    std::atomic_bool start{false};                                      //!< Start collecting data
    // Configuration, other-config REST call
    period_type save_interval{131000};                                     //!< Histogram saving period: ~1s for TDC frequency 131kHz
    TimeRoi time_roi;                                                      //!< Time ROI
    std::string output_uri;                                                //!< file:name (without period and .xes), or tcp:host:port
    // Configuration, pixel-map(-from-file) REST call
    std::unique_ptr<PixelMap> pix_map{nullptr};                            //!< Area ROI

    /*!
    \brief Acquire configuration lock

    In state `config`, this lock must be aquired to access
    the variables configurarable via REST calls.
    \return Lock guard object
    */
    static std::lock_guard<std::mutex> configLock();

    // From CLI arguments
    bool server_mode{false};                                               //!< Run program in server mode (from commandline arg)
    SocketAddress serverAddress = SocketAddress{"localhost:8080"};         //!< Default ASI server address
    SocketAddress clientAddress = SocketAddress{"127.0.0.1:8451"};         //!< Default raw data stream tcp destination (own address)
    SocketAddress controlAddress = SocketAddress{"127.0.0.1:8452"};        //!< Default control interface address (own address)

    // From ASI server
    detector_layout layout;                                                //!< Detector layout (retrieved from ASI server)

    // From CLI arguments
    cpu_mask::cpu_mask_t cpu_affinity;                                     //!< CPU affinity for reader,writer, analysis threads

    // From code
    static constexpr bool reset_error = true;                               //!< Reset last know error
    static constexpr bool no_error_reset = false;                           //!< Don't reset last know error
    /*!
    \brief Set last known error
    \param error Error message, default = no error
    */
    static void set_error(const std::string& error = "") noexcept;

    /*!
    \brief Get and reset last known error
    \param reset Reset last know error iff true
    \return Last know error
    */
    static std::string get_error(bool reset) noexcept;

    /*!
    \brief Check for empty last know error
    \return True if there is no last know error
    */
    static bool error_empty() noexcept;

    // program state: init -> config -> setup -> await_connection -> collect (-> config..) -> shutdown
    static constexpr std::string_view init{"init"};                         //!< initial state
    static constexpr std::string_view config{"config"};                     //!< ready for configuration, from init
    static constexpr std::string_view setup{"setup"};                       //!< config done, setup data collection, from config
    static constexpr std::string_view await_connection{"await_connection"}; //!< wait for ASI server to connect, from setup
    static constexpr std::string_view collect{"collect"};                   //!< collect data, from await_connection
    static constexpr std::string_view except{"except"};                     //!< exception happened, from config, setup, await_connection, collect
    static constexpr std::string_view shutdown{"shutdown"};                 //!< program shutdown, from config, setup, collect

    std::string_view state{init};                                                //!< program state (TODOD: protect with lock, if necessary)

    // Singleton
    static std::unique_ptr<global> instance;                               //!< unique instance
};

#endif
