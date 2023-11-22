#ifndef PROCESSING_H
#define PROCESSING_H

/*!
\file
Includes for processing code
*/

#include "layout.h"

namespace processing {

    /*!
    \brief Initialize the event analysis code

    This function must be called before any other functions.
    The "Processing.ini" file in the current directory will be parsed, and
    corresponding Detector and Analysis objects will be created.

    \param layout The detector layout
    */
    void init(const detector_layout& layout);

    /*!
    \brief Purge an old period change interval off the period queue

    This must only be done if no event for this period change interval will be encountered.

    \param chipIndex    Purge period for this chip only
    \param period       The period change at the beginning of this period will be purged off the queue
    */
    void purgePeriod(unsigned chipIndex, period_type period);

    /*!
    \brief Process a TOA event
    \param chipIndex        Event was on this chip
    \param period           Period of the event
    \param toaclk           The absolute TOA for the event
    \param relative_toaclk  The relative TOA for the event, relative to the last TDC event
    \param event            The raw event data
    */
    void processEvent(unsigned chipIndex, const period_type period, int64_t toaclk, int64_t relative_toaclk, uint64_t event);

} // namespace processing

#endif
