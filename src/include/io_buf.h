#pragma once

#include <cstddef>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cassert>
#include <vector>
#include <chrono>

#include <unistd.h>
#include <sys/mman.h>

#include "Poco/Exception.h"

namespace iobuf {
    using namespace std::chrono_literals;
    inline int container_size = 4 * sysconf(_SC_PAGE_SIZE);

    struct container_t final {
        [[gnu::aligned(256/8)]] char* data;
        bool pinned;

        inline container_t()
            : pinned{false}
        {
            data = (char*)mmap(nullptr, container_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_POPULATE, -1 , 0);
            if (data == MAP_FAILED)
                throw Poco::SystemException("internal - mmap failed", errno);
        }

        inline ~container_t() noexcept
        {
            munmap(data, container_size);
        }

        inline void pin()
        {
            if (pinned)
                return;

            if (!data)
                return;
            
            if (mlock(data, container_size))
                throw Poco::SystemException("internal - mlock failed", errno);
            pinned = true;
        }

        inline void unpin()
        {
            if (!pinned)
                return;

            if (munlock(data, container_size))
                throw Poco::SystemException("internal - munlock failed", errno);

            pinned = false;
        }
    };

    struct jar_t final {
        container_t container;
        jar_t* next;                // singly linked list, if level==container_size => next is set
        std::atomic<unsigned> done; // done count, if done==nthreads => next was consumed
        int level;                  // protect with lock and condvar
        std::mutex level_lock;
        std::condition_variable level_cond;

        jar_t() noexcept
            : next{nullptr}, done{0u}, level{0}
        {}

        explicit jar_t(jar_t* next_) noexcept
            : next{next_}, done{0u}, level{0}
        {}
    };

    struct reservation_t final {
        jar_t* jar;   // singly linked list
        int start;
        int end;
    };


    inline reservation_t initial_reservation = {nullptr, 0, 0};

    class collection_t final {
        static constexpr unsigned num_initial_containers = 8u;
        std::mutex free_lock;
        std::vector<std::unique_ptr<jar_t>> level_list;
        jar_t* head;            // singly linked free list
        jar_t* tail;            // singly linked free list
        jar_t* first;           // initial write
        jar_t* final;           // final data
        std::atomic<bool> stop; // stop operation immediately
        const unsigned nthreads;// number of reader threads

        inline int await_data(jar_t* jar, int level)
        {
            std::unique_lock lock{jar->level_lock};
            do {
                if (jar->level != level)
                    return jar->level;
                if ((jar == final) || stop.load(std::memory_order_consume))
                    return 0;
                jar->level_cond.wait_for(lock, 1s);
            } while (true);
        }

    public:
        explicit collection_t(unsigned threads)
            : nthreads{threads}
        {
            level_list.resize(num_initial_containers);
            head = level_list[1].get();
            for (unsigned i=1u; i<num_initial_containers-1u; i++)
                level_list[i]->next = level_list[i+1].get();
            tail = level_list[num_initial_containers-1u].get();
            first = level_list[0].get();
            final = nullptr;
        }

        inline void stop_now()
        {
            stop.store(true, std::memory_order_release);
        }

        inline reservation_t write_reservation(const reservation_t& reservation, int size)
        {
            jar_t* jar = reservation.jar;
            const auto end = reservation.start + size;
            assert((end >= reservation.start) && (end <= container_size));
            if (!jar) {
                // initial container
                assert(first);
                return {first, 0, container_size};
            }
            if (!size || stop.load(std::memory_order_consume)) {
                // finished
                {
                    std::lock_guard lock{jar->level_lock};
                    final = jar;
                    jar->level_cond.notify_all();
                }
                return {jar, end, 0};
            }
            if (end == container_size) {
                // finished with container
                jar_t* free = nullptr;
                {
                    {
                        std::lock_guard lock{free_lock};
                        if (head) {
                            free = head;
                            head = head->next;
                            if (tail == free)
                                tail = nullptr;
                        }
                    }
                    free->next = nullptr;
                }
                if (!free) {
                    // create new container
                    level_list.emplace_back(new jar_t);
                    free = level_list.back().get();
                }
                assert(free && !free->next && !free->done && !free->level);
                {
                    std::lock_guard lock{jar->level_lock};
                    jar->next = free;
                    jar->level = end;
                    jar->level_cond.notify_all();
                }
                return {free, 0, container_size};
            }
            // not jet finished with container
            {
                std::lock_guard lock{jar->level_lock};
                jar->level = end;
                jar->level_cond.notify_all();
            }
            return {jar, end, container_size};
        }

        inline reservation_t read_reservation(const reservation_t& reservation)
        {
            jar_t* jar = reservation.jar;
            const auto end = reservation.end;
            if (!jar) {
                // initial container
                assert(first);
                return {first, 0, await_data(first, 0)};
            } else if (end == container_size) {
                // finished with container
                jar_t* next = jar->next;
                assert(next);
                if ((jar->done += 1u) == nthreads) {
                    // all threads have finished here
                    jar->next = nullptr;
                    jar->done = 0u;
                    jar->level = 0;
                    {
                        std::lock_guard lock{free_lock};
                        if (tail)
                            tail->next = jar;
                        else
                            head = jar;
                        tail = jar;
                    }
                }
                return {next, 0, await_data(next, 0)};
            }
            // not jet finished with container
            assert((end > 0) && (end < container_size));
            return {jar, end, await_data(jar, end)};
        }
    };
} // namespace iobuf
