#pragma once

#ifndef PROCESSING_H
#define PROCESSING_H

/*!
\file
Includes for processing code
*/

#include "shared_types.h"
#include "layout.h"

namespace processing {

    /*!
    \brief Initialize the event analysis code

    This function must be called before any other functions.
    The "Processing.ini" file in the current directory will be parsed, and
    corresponding Detector and Analysis objects will be created.
    */
    void init();

} // namespace processing

#endif
