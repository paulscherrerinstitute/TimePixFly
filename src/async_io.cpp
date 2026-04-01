#if defined(NO_IOURING)
    #include "unistd.h"
#endif
#include "async_io.h"

namespace async {

    void handle::release() noexcept
    {
        #if defined(NO_IOURING)
            if (ring) {
                ring->release(*this);
                ring = nullptr;
                res = -1;
            }
        #else
            if (cqe) {
                assert(ring);
                ring->release(*this);
                ring = nullptr;
                cqe = nullptr;
            }
        #endif
    }

    void uring::release(handle& this_handle) noexcept
    #if defined(NO_IOURING)
        {
            queue.erase(this_handle.id64);
        }
    #else
        {
            assert(this_handle.cqe == next_cqe);
            io_uring_cqe_seen(&ring, this_handle.cqe);
            next_cqe = nullptr;
        }
    #endif

    bool uring::enqueue_read(int fd, void* buf, unsigned nbytes, u64 offset, u64 id)
    {
        #if defined(NO_IOURING)
            if (queue.size() >= queue_size)
                return false;
            int res = read(fd, (char*)buf + offset, nbytes);
            if (res < 0)
                res = -errno;
            queue.emplace(id, res);
            enqueued++;
        #else
            auto* sqe = get_sqe();
            if (sqe == nullptr)
                return false;

            io_uring_prep_read(sqe, fd, buf, nbytes, offset);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(id));
        #endif
        return true;
    }

    handle uring::wait()
    {
        #if defined(NO_IOURING)
            if (queue.empty())
                throw Poco::RuntimeException("io queue empty");
            auto it = queue.begin();
            return { this, it->second, it->first };
        #else
            assert(next_cqe == nullptr);
            io_uring_cqe* cqe;
            int res  = io_uring_wait_cqe(&ring, &cqe);
            if (res < 0)
                throw Poco::SystemException(-res);

            next_cqe = cqe;
            return {this, cqe};
        #endif
    }
}