#ifndef PROCESSING_H
#define PROCESSING_H

#include "layout.h"

namespace processing {

    void init(const detector_layout& layout);
    void purgePeriod(unsigned chipIndex, period_type period);
    void processEvent(unsigned chipIndex, const period_type period, int64_t toaclk, uint64_t event);

} // namespace processing

#endif
