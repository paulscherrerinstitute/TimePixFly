#pragma once

/*!
\file Provide asynchronous read/write functionality via io_uring
*/

#include <cassert>
#include <cstddef>
#include <map>

#if !defined(NO_IOURING)
    #include <liburing.h>
#endif

#include <Poco/Exception.h>

#include "shared_types.h"

namespace async {
    class uring;

    class handle final {
        uring* ring = nullptr;
        #if defined(NO_IOURING)
            int res = -1;
            u64 id64;
        #else
            io_uring_cqe* cqe = nullptr;
        #endif

    public:
        inline handle() noexcept = default;

        #if defined(NO_IOURING)
            inline handle(uring* ready_ring, int ready_res, u64 ready_id) noexcept
                : ring{ready_ring}, res{ready_res}, id64{ready_id}
            {
                assert(ring);
            }
        #else
            inline handle(uring* ready_ring, io_uring_cqe* ready_cqe) noexcept
                : ring{ready_ring}, cqe{ready_cqe}
            {
                assert(ring && cqe);
            }
        #endif

        handle(const handle&) = delete;
        handle& operator=(const handle&) = delete;

        inline handle(handle&& other) noexcept
        {
            std::swap(ring, other.ring);
            #if defined(NO_IOURING)
                std::swap(res, other.res);
                std::swap(id64, other.id64);
            #else
                std::swap(cqe, other.cqe);
            #endif
        }

        void release() noexcept;

        inline handle& operator=(handle&& other) noexcept
        {
            release();
            std::swap(ring, other.ring);
            #if defined(NO_IOURING)
                std::swap(res, other.res);
                std::swap(id64, other.id64);
            #else
                std::swap(cqe, other.cqe);
            #endif
            return *this;
        }

        inline ~handle() noexcept
        {
            release();
        }

        bool invalid() const noexcept
        {
            #if defined(NO_IOURING)
                return res < 0;
            #else
                return cqe == nullptr;
            #endif
        }

        int result() const noexcept
        {
            #if defined(NO_IOURING)
                return res;
            #else
                assert(cqe);
                return cqe->res;
            #endif
        }

        u64 id() const noexcept
        {
            #if defined(NO_IOURING)
                return id64;
            #else
                assert(cqe);
                return io_uring_cqe_get_data64(cqe);
            #endif
        }

        friend class uring;
    };

    // From liburing.h
    //
    // struct io_uring {
    //     struct io_uring_sq sq;
    //     struct io_uring_cq cq;
    //     unsigned int flags;
    //     int ring_fd;
    //     unsigned int features;
    //     int enter_ring_fd;
    //     __u8 int_flags;
    //     __u8 pad[3];
    //     unsigned int pad2;
    // };

    class uring final {
        #if defined(NO_IOURING)
            std::map<u64, int> queue;
            unsigned queue_size = 0;
            unsigned enqueued = 0;
        #else
            io_uring_cqe* next_cqe = nullptr;
            io_uring ring;
            bool is_initialized = false;

            inline io_uring_sqe* get_sqe() noexcept
            {
                return io_uring_get_sqe(&ring);
            }
        #endif

        void release(handle& this_handle) noexcept;

    public:
        inline explicit uring(unsigned num_entries, [[maybe_unused]] uring *couple_with=nullptr)
        {
            #if defined(NO_IOURING)
                queue_size = num_entries;
            #else
                // TODO
                // 1) Set io_uring_params.wq_fd to couple_with.ring.ring_fd with
                // IORING_SETUP_ATTACH_WQ flag set, to share worker threads
                // 2) Tune io_uring_params.cq_entries
                int res = io_uring_queue_init(num_entries, &ring, 0);
                if (res < 0)
                    throw Poco::SystemException(-res);
                is_initialized = true;
            #endif
        }

        inline uring(uring&& other) noexcept
        #if defined(NO_IOURING)
            = default;
        #else
                : ring(other.ring)
            {
                std::swap(is_initialized, other.is_initialized);
            }
        #endif

        inline uring &operator=(uring&& other) noexcept
        #if defined(NO_IOURING)
            = default;
        #else
        {
            ring = other.ring;
            std::swap(is_initialized, other.is_initialized);
            return *this;
        }
        #endif

        uring(const uring &) = delete;
        uring &operator=(const uring &) = delete;

        inline ~uring() noexcept
        {
            #if !defined(NO_IOURING)
                if (is_initialized) {
                    io_uring_queue_exit(&ring);
                    is_initialized = false;
                }
            #endif
        }

        bool enqueue_read(int fd, void* buf, unsigned nbytes, u64 offset, u64 id);

        inline int submit() noexcept
        {
            #if defined(NO_IOURING)
                int rval = enqueued;
                enqueued = 0;
                return rval;
            #else
                return io_uring_submit(&ring);
            #endif
        }

        handle wait();

        friend class handle;
    };

} // namespace async

