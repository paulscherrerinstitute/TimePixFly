#pragma once

/*!
\file Provide asynchronous read/write functionality via io_uring
*/

#include <cassert>
#include <cstddef>
#include <vector>
#include <liburing.h>

#include <Poco/Exception.h>

#include "shared_types.h"

namespace async {
    class uring;

    class handle final {
        uring* ring = nullptr;
        io_uring_cqe* cqe = nullptr;

    public:
        inline handle() noexcept = default;

        inline handle(uring* ready_ring, io_uring_cqe* ready_cqe) noexcept
            : ring{ready_ring}, cqe{ready_cqe}
        {
            assert(ring && cqe);
        }

        handle(const handle&) = delete;
        handle& operator=(const handle&) = delete;

        inline handle(handle&& other) noexcept
        {
            std::swap(ring, other.ring);
            std::swap(cqe, other.cqe);
        }

        void release() noexcept;

        inline handle& operator=(handle&& other) noexcept
        {
            release();
            std::swap(ring, other.ring);
            std::swap(cqe, other.cqe);
            return *this;
        }

        inline ~handle() noexcept
        {
            release();
        }

        bool invalid() const noexcept
        {
            return cqe == nullptr;
        }

        int result() const noexcept
        {
            assert(cqe);
            return cqe->res;
        }

        u64 id() const noexcept
        {
            assert(cqe);
            return io_uring_cqe_get_data64(cqe);
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
        io_uring_cqe* next_cqe = nullptr;
        io_uring ring;
        bool is_initialized = false;

        inline io_uring_sqe* get_sqe() noexcept
        {
            return io_uring_get_sqe(&ring);
        }

        void release(handle& this_handle) noexcept;

    public:
        inline explicit uring(unsigned num_entries, [[maybe_unused]] uring *couple_with=nullptr)
        {
            // TODO
            // 1) Set io_uring_params.wq_fd to couple_with.ring.ring_fd with
            // IORING_SETUP_ATTACH_WQ flag set, to share worker threads
            // 2) Tune io_uring_params.cq_entries
            int res = io_uring_queue_init(num_entries, &ring, 0);
            if (res < 0)
                throw Poco::SystemException(-res);
            is_initialized = true;
        }

        inline uring(uring&& other) noexcept
            : ring(other.ring)
        {
            std::swap(is_initialized, other.is_initialized);
        }

        inline uring &operator=(uring&& other) noexcept
        {
            ring = other.ring;
            std::swap(is_initialized, other.is_initialized);
            return *this;
        }

        uring(const uring &) = delete;
        uring &operator=(const uring &) = delete;

        inline ~uring() noexcept
        {
            if (is_initialized) {
                io_uring_queue_exit(&ring);
                is_initialized = false;
            }
        }

        bool enqueue_read(int fd, void* buf, unsigned nbytes, u64 offset, u64 id);

        inline int submit() noexcept
        {
            return io_uring_submit(&ring);
        }

        handle wait();

        friend class handle;
    };

} // namespace async

