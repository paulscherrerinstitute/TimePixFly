#include "async_io.h"

namespace async {

    void handle::release() noexcept
    {
        if (cqe) {
            assert(ring);
            ring->release(*this);
            ring = nullptr;
            cqe = nullptr;
        }
    }

    void uring::release(handle& this_handle) noexcept
    {
        assert(this_handle.cqe == next_cqe);
        io_uring_cqe_seen(&ring, this_handle.cqe);
        next_cqe = nullptr;
    }

    bool uring::enqueue_read(int fd, void* buf, unsigned nbytes, u64 offset, u64 id)
    {
        auto* sqe = get_sqe();
        if (sqe == nullptr)
            return false;

        io_uring_prep_read(sqe, fd, buf, nbytes, offset);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(id));
        return true;
    }

    handle uring::wait()
    {
        assert(next_cqe == nullptr);
        io_uring_cqe* cqe;
        int res  = io_uring_wait_cqe(&ring, &cqe);
        if (res < 0)
            throw Poco::SystemException(-res);

        next_cqe = cqe;
        return {this, cqe};
    }
}