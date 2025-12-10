#pragma once

#ifndef CONFIG_FILE_H
#define CONFIG_FILE_H

#include "Poco/Util/IniFileConfiguration.h"

/*!
\brief Processing configuration file object
*/
struct ConfigFile final : public Poco::Util::IniFileConfiguration {
        /*!
        \brief Constructor
        \param path INI style configuration file path
        */
        inline ConfigFile(const std::string& path)
                : IniFileConfiguration{path}
        {}

        inline ~ConfigFile() noexcept = default;
};

#endif // CONFIG_FILE_H
