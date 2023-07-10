#ifndef PERIOD_QUEUES_H
#define PERIOD_QUEUES_H

#include <memory>
#include <map>
#include "event_reordering.h"

struct period_queue_element final {
    std::unique_ptr<event_reorder_queue> queue;
    int64_t start;
    bool start_seen = false;    // either start is valid, or the queue, but not both
};

struct period_index final {
    long period;
    long disputed_period;
    bool disputed;
};

struct period_queues final {

    [[gnu::const]]
    inline period_index period_index_for(double period)
    {
        double f = period - std::floor(period);
        long p = std::lround(period);
        if (f > 1. - threshold)
            return { p, p+1, true };
        if (f < threshold)
            return { p, p, true };
        return { p, p, false };
    }

    inline void refined_index(period_index& to_refine, uint64_t time_stamp)
    {
        if (! to_refine.disputed)
            return;

        auto& pqe_ptr = element.find(to_refine.disputed_period);
        if (pqe_ptr == std:end(element))
            return;

        if (! pqe_ptr->start_seen)
            return;
        
        to_refine.disputed = false;

        if (pqe_ptr->period == pqe_ptr->disputed_period) {  // disputed at start
            if (pqe_ptr->start > time_stamp)
                to_refine.period -= 1;
        } else {                                            // disputed at end
            if (pqe_ptr->start <= time_stamp)
                to_refine.period += 1;
        }

        return;
    }

    inline period_queue_element& operator[](const period_index& idx)
    {
        auto& pqe = element[idx.disputed ? idx.disputed_period : idx.period];
        if (pqe.queue == nullptr)
            pqe.queue.reset(new period_queue_element{})
        return pqe;
    }

    std::map<long, period_queue_element> element;   // key = period number
    double threshold = 0.1;                         // (1 - threshold) is the undisputed period attribution interval
};

#endif // PERIOD_QUEUES_H