#ifdef IO_BUFFERS_H
#define IO_BUFFERS_H

#include <map>
#include <list>
#include <mutex>
#include <condition_variable>

struct rw_buffer {
    std::vector<char> content;
    size_t size = 0;

    rw_buffer(size_t sz)
        : content{sz}
    {}

    rw_buffer(const rw_buffer&) = delete;
    rw_buffer(rw_buffer&&) = default;
    rw_buffer& operator=(const rw_buffer&) = delete;
    rw_buffer& operator=(rw_buffer&&) = default;
}

struct io_buffers final {
    static size_t buffer_size = 1024;
    using buffer_type = std::map<uint64_t, std::unique_ptr<rw_buffer>>;
    using element_type = buffer_type::value_type;
    buffer_type buffer;
    std::list<std::unique_ptr<rw_buffer>> free_list;
    std::mutex modify_buffer;
    std::condition_variable ready_for_reading;
    std::mutex modify_free_list;
    bool no_more_data = false;

    // block on condvar if buffer empty and no_more_data is false
    // rw_buffer pointer is nullptr if there is no more data
    inline element_type get_nonempty_buffer()
    {
        std::lock_guard lock(modify_buffer);
        while (buffer.empty()) {
            if (no_more_data)
                return {0, nullptr};
            ready_for_reading.wait()
        }
        auto top = std::begin(buffer);
        element_type res = std::move(*top);
        buffer.erase(top);
        return res;
    }

    inline void put_empty_buffer(std::unique_ptr<rw_buffer>&& buf)
    {
        std::lock_guard(modify_free_list);
        free_list.push_front(buf);
    }

    inline std::unique_ptr<rw_buffer> get_empty_buffer()
    {
        std::lock_guard(modify_free_list);
        auto top = std::begin(free_list);
        if (top = std::end(free_list))
            return new rw_buffer{buffer_size};
        auto res = std::move(*top);
        free_list.pop_front();
        return res;
    }

    inline void put_nonfree_buffer(element_type&& element)
    {
        std::lock_guard lock(modify_buffer);
        buffer.insert(element);
        ready_for_reading.notify_one();
    }

    inline void finish_writing()
    {
        std::lock_guard lock(modify_buffer);
        no_more_data = true;
        ready_for_reading.notify_one();
    }

    io_buffers() = default;
    io_buffers(const io_buffers&) = delete;
    io_buffers(io_buffers&&) = default;
    io_buffers& operator=(const io_buffers&) = delete;
    io_buffers& operator=(io_buffers&&) = default;
};

using io_buffer_collection = std::vector<std::unique_ptr<io_buffers>>;

#endif // IO_BUFFERS_H
