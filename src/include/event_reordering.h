#ifndef EVENT_REORDERING_H
#define EVENT_REORDERING_H

/*!
\file
Code for event reordering based on event TOA
*/

#include <cstdint>
#include "reorder_queue.h"

/*!
\brief Reorder element representation for one TOA event
*/
struct reordering_element final {
    int64_t toa;    //!< TOA is the reordering priority
    uint64_t event; //!< Raw event

    /*!
    \brief Constructor
    \param toa_     TOA of event that participated in reordering
    \param event_   Raw event that participated in reordering
    */
    inline reordering_element(int64_t toa_, uint64_t event_) noexcept
        : toa{toa_}, event{event_}
    {}

    inline ~reordering_element() = default;
    inline reordering_element(const reordering_element&) = default; //!< Copy constructor
    inline reordering_element(reordering_element&&) = default;      //!< Move constructor
    inline reordering_element& operator=(const reordering_element&) = default;  //!< Copy assignment \return `this`
    inline reordering_element& operator=(reordering_element&&) = default;       //!< Move assignment \return `this`
};

/*!
\brief Reordering element priority comparator based on TOA
*/
struct toa_older_comparator final {
    /*!
    \brief Comparison operator
    \param lhs
    \param rhs
    \return True iff lhs > rhs
    */
    inline bool operator()(const reordering_element& lhs, const reordering_element& rhs) const noexcept
    {
        return lhs.toa > rhs.toa;
    }
};

/*!
\brief Event reordering queue type

The event reordering queue is a priority queue containing objects of
type `reordering_element`that get ordered by TOA.
*/
using event_reorder_queue = reorder_queue<reordering_element, toa_older_comparator>;

#endif // EVENT_REORDERING_H
