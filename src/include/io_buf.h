#pragma once

#ifndef IO_BUF_H
#define IO_BUF_H

/*!
\file
Provide a single produce, multiple consumer I/O buffer implementation
*/

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

/*!
\brief I/O Buffer implementation
*/
namespace iobuf {
    using namespace std::chrono_literals;

    /*!
    I/O buffer size

    WARNING:
    Set this value before using any of the classes in this namespace,
    and do NOT change it afterwards!

    This value is assumed to be constant within the iobuf code.
    */
    inline int container_size = 8 * sysconf(_SC_PAGE_SIZE);

    /*!
    \brief Data container
    The data is page size aligned can be pinned down in memory.
    */
    struct container_t final {
        [[gnu::aligned(256/8)]] char* data; //!< Aligned data
        bool pinned;                        //!< Is the data pinned down in memory?

        /*!
        \brief Create unpinned data container
        */
        inline container_t()
            : pinned{false}
        {
            data = (char*)mmap(nullptr, container_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_POPULATE, -1 , 0);
            if (data == MAP_FAILED)
                throw Poco::SystemException("internal - mmap failed", errno);
        }

        /*!
        \brief Destructor
        */
        inline ~container_t() noexcept

        {
            unpin();
            munmap(data, container_size);
        }

        container_t(const container_t&) = delete;               //!< no copying
        container_t& operator=(const container_t&) = delete;    //!< no copying

        /*!
        \brief Pin data down in memory
        */
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

        /*!
        \brief Remove memory pin
        */
        inline void unpin()
        {
            if (!pinned)
                return;

            if (munlock(data, container_size))
                throw Poco::SystemException("internal - munlock failed", errno);

            pinned = false;
        }
    };

    /*!
    \brief Linked data containers with thread snyhronization infrastructure and fill level
    */
    struct jar_t final {
        container_t container;              //!< Data container
        jar_t* next;                        //!< Singly linked list, if level==container_size => next is set
        std::atomic<unsigned> done;         //!< Threads done count, if done==nthreads => next was consumed
        int level;                          //!< Fill level
        std::mutex level_lock;              //!< Protect fill level
        std::condition_variable level_cond; //!< For awaiting fill level changes

        /*!
        \brief Construct a new unlinked jar with fill level 0
        */
        inline jar_t() noexcept
            : next{nullptr}, done{0u}, level{0}
        {}

        /*!
        \brief Construct a new linked jar with fill level 0
        \param next_ Link to next jar
        */
        inline explicit jar_t(jar_t* next_) noexcept
            : next{next_}, done{0u}, level{0}
        {}
    };

    /*!
    \brief Output operator for a jar
    \param os Output stream
    \param jar Jar to print into the stream
    \tparam out Output stream type
    \return Output stream
    */
    template<typename out>
    inline out& operator<<(out& os, const jar_t& jar)
    {
        return os << "(jar " << &jar << " next=" << jar.next << " done=" << jar.done << " level=" << jar.level << ')';
    }

    /*!
    \brief Thread reservation for jar content
    */
    struct reservation_t final {
        jar_t* jar; //!< Jar in question, no reservation if null
        int start;  //!< Reservation start level
        int end;    //!< Reservation end level
    };

    inline reservation_t initial_reservation = {nullptr, 0, 0}; //!< Initially there is no reservation

    /*!
    \brief Reservation output operator
    \param os Output stream
    \param res Reservation to print into the output stream
    \tparam out Output stream type
    \return Output stream
    */
    template<typename out>
    inline out& operator<<(out& os, const reservation_t& res)
    {
        return os << "(res " << &res.jar << " start=" << res.start << " end=" << res.end << ')';
    }

    /*!
    \brief Collection of data containers for one producer and a fixed number of consumers
    */
    class collection_t final {
        static constexpr unsigned num_initial_containers = 8u;  //!< Initial amount of jars
        std::mutex free_lock;                                   //!< Protect free list of empty, reusable jars
        std::vector<std::unique_ptr<jar_t>> jar_list;           //!< Vector of data jars
        jar_t* head;                                            //!< Head of singly linke free list
        jar_t* tail;                                            //!< Tail of singly linked free list
        jar_t* first;                                           //!< Initially used jar
        std::atomic<jar_t*> final_jar;                          //!< The final jar that was filled up
        std::atomic<bool> stop_flag;                            //!< Irregular stop
        const unsigned nthreads;                                //!< Number of consumer threads
        bool pin_data;                                          //!< Pin data to memory

        /*!
        \brief Await jar fill level change
        \param jar For this jar
        \param level Old fill level
        \return Changed fill level
        */
        inline int await_data(jar_t* jar, int level)
        {
            assert(jar);
            std::unique_lock lock{jar->level_lock};
            do {
                if (jar->level != level)
                    return jar->level;
                if ((jar == final_jar.load(std::memory_order_consume)) || stop_flag.load(std::memory_order_consume))
                    return 0;
                jar->level_cond.wait_for(lock, 1s);
            } while (true);
        }

    public:
        /*!
        \brief Create data container collection for a fixed number of consumer threads
        All threads must consume each container
        \param threads Number of consumer threads
        \param pinned Pin data to memory
        */
        inline explicit collection_t(unsigned threads, bool pinned=true)
            : final_jar{nullptr}, stop_flag{false}, nthreads{threads}, pin_data(pinned)
        {
            jar_list.resize(num_initial_containers);
            for (auto& p : jar_list) {
                p.reset(new jar_t);
                if (pin_data)
                    p->container.pin();
            }
            head = jar_list[1].get();
            for (unsigned i=1u; i<num_initial_containers-1u; i++)
                jar_list[i]->next = jar_list[i+1].get();
            tail = jar_list[num_initial_containers-1u].get();
            first = jar_list[0].get();
        }

        /*!
        \brief Irregular stop
        */
        inline void stop_now() noexcept
        {
            stop_flag.store(true, std::memory_order_release);
        }

        /*!
        \brief Get initial jar
        \return Initial jar
        */
        inline jar_t* first_jar()
        {
            assert(first);
            return first;
        }

        /*!
        \brief Get the next jar with respect to `prev`

        This will link `prev->next` to the returned jar.
        \param prev Jar that will be the previous one after return
        \return Next jar
        */
        inline jar_t* next_jar(jar_t* prev)
        {
            assert(prev && !prev->next);
            jar_t* free = nullptr;
            {
                std::lock_guard lock{free_lock};
                if (head) {
                    free = head;
                    head = head->next;
                    if (tail == free)
                        tail = nullptr;
                }
            }
            if (!free) {
                // create new container
                jar_list.emplace_back(new jar_t);
                free = jar_list.back().get();
                if (pin_data)
                    free->container.pin();
            } else {
                free->next = nullptr;
            }
            prev->next = free;
            return free;
        }

        /*!
        \brief Return jar to free list
        \param jar Jar to return
        */
        inline void return_jar(jar_t* jar)
        {
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
        }

        /*!
        \brief Commit data to jar
        \param jar Commit data in this jar
        \param level The new fill level
        */
        inline void put_data(jar_t* jar, int level)
        {
            assert(jar);
            std::lock_guard lock{jar->level_lock};
            
            if (jar->level == level)
                final_jar.store(jar, std::memory_order_release);
            else
                jar->level = level;

            jar->level_cond.notify_all();
        }

        /*!
        \brief Get a write reservation for the single producer thread.

        This will take an empty jar from the free list, or create a new jar.
        \param consumed Reservation that has been consumed, or `initial_reservation`<br/>
            If the consumed reservation has `start == end`, that signals the final
            jar was filled up and no more calls are allowed.
        \return New reservation<br/>
            If `end == 0`, no more calls are allowed.
        */
        inline reservation_t write_reservation(const reservation_t& consumed)
        {
            jar_t* jar = consumed.jar;
            const auto end = consumed.end;
            assert((end >= consumed.start) && (end <= container_size));

            if (!jar) {
                // initial container
                assert(first);
                return {first, 0, container_size};
            }

            if ((consumed.start == end) || stop_flag.load(std::memory_order_consume)) {
                // finished
                {
                    std::lock_guard lock{jar->level_lock};
                    final_jar.store(jar, std::memory_order_release);
                    jar->level_cond.notify_all();
                }
                // std::ostringstream oss;
                // oss << "final=" << *jar << ", end=" << end << '\n';
                // std::cout << oss.str();
                return {jar, end, 0};
            }

            if (end == container_size) {
                // finished with container
                jar_t* free = next_jar(jar);
                assert(free && !free->done && !free->level && (jar->next == free));
                put_data(jar, end);
                return {free, 0, container_size};
            }

            // not jet finished with container
            put_data(jar, end);
            return {jar, end, container_size};
        }

        /*!
        \brief Get a read reservation for one of the consumer threads

            This will link the link the jar to the free list, if all threads are done with it.
        \param consumed Reservation that has been consumed, or `initial_reservation`<br/>
            If `end == container_size`, the thread is done with the jar.
        \return New reservation<br/>
            If `end == 0`, no mor data is produced, and no more calls are allowed.
        */
        inline reservation_t read_reservation(const reservation_t& consumed, bool no_return=false)
        {
            jar_t* jar = consumed.jar;
            const auto end = consumed.end;
            int level;
            if (!jar) {
                // initial container
                assert(first);
                level = await_data(first, 0);
                return {first, 0, level};
            } else if (end == container_size) {
                // finished with container
                jar_t* next = jar->next;
                assert(next);
                if (__builtin_expect(!no_return, 1))
                    return_jar(jar);
                level = await_data(next, 0);
                return {next, 0, level};
            }
            // not jet finished with container
            assert((end > 0) && (end < container_size));
            level = await_data(jar, end);
            return {jar, end, level};
        }
    };
} // namespace iobuf

#endif // ifndef IO_BUF_H
