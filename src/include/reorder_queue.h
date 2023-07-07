#ifndef REORDER_QUEUE_H
#define REORDER_QUEUE_H

// Provide code for a reorder queue

// Design:
// Priority queue

// TODO: check if fixed capacity queue implementation is faster

#include <queue>

// Simple priority queue wrapper
template<typename T, typename Compare>
using reorder_queue = std::priority_queue<T, std::vector<T>, Compare>;

#endif // REORDER_QUEUE_H
