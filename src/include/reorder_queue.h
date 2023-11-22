#ifndef REORDER_QUEUE_H
#define REORDER_QUEUE_H

/*!
\file
Provide code for a reorder queue

Design: Priority queue
*/

#include <queue>

/*!
\brief Simple priority queue wrapper
*/
template<typename T, typename Compare>
using reorder_queue = std::priority_queue<T, std::vector<T>, Compare>;

#endif // REORDER_QUEUE_H
