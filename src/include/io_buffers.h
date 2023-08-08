#ifndef IO_BUFFERS_H
#define IO_BUFFERS_H

#include <map>
#include <list>
#include <mutex>
#include <condition_variable>

struct io_buffer final {
    inline static std::atomic<unsigned> next_id;
    std::vector<char> content;
    size_t content_offset = 0;
    size_t content_size = 0;
    size_t chunk_size = 0;
    unsigned id;

    io_buffer(size_t sz)
        : content(sz), id{next_id.fetch_add(1)}
    {}

    io_buffer(const io_buffer&) = delete;
    io_buffer(io_buffer&&) = default;
    io_buffer& operator=(const io_buffer&) = delete;
    io_buffer& operator=(io_buffer&&) = default;
};

struct io_buffer_pool final {
    inline static size_t buffer_size = 1024;
    using buffer_type = std::multimap<uint64_t, std::unique_ptr<io_buffer>>;
    using element_type = buffer_type::value_type;
    buffer_type buffer;
    std::list<std::unique_ptr<io_buffer>> free_list;
    std::mutex modify_buffer;
    std::condition_variable ready_for_reading;
    std::mutex modify_free_list;
    bool no_more_data = false;

    // block on condvar if buffer empty and no_more_data is false
    // io_buffer pointer is nullptr if there is no more data
    inline element_type get_nonempty_buffer()
    {
        std::unique_lock lock(modify_buffer);
        while (buffer.empty()) {
            if (no_more_data)
                return {0, nullptr};
            ready_for_reading.wait(lock);
        }
        auto top = std::begin(buffer);
        element_type res = std::move(*top);
        buffer.erase(top);
        return res;
    }

    inline void put_empty_buffer(std::unique_ptr<io_buffer>&& buf)
    {
        std::lock_guard lock(modify_free_list);
        free_list.push_front(std::move(buf));
    }

    inline std::unique_ptr<io_buffer> get_empty_buffer()
    {
        std::lock_guard lock(modify_free_list);
        auto top = std::begin(free_list);
        if (top == std::end(free_list))
            return std::unique_ptr<io_buffer>(new io_buffer{buffer_size});
        auto res = std::move(*top);
        free_list.pop_front();
        res->content_size = 0;
        return res;
    }

    inline void put_nonempty_buffer(element_type&& element)
    {
        std::lock_guard lock(modify_buffer);
        buffer.insert(std::move(element));
        ready_for_reading.notify_one();
    }

    inline void finish_writing()
    {
        std::lock_guard lock(modify_buffer);
        no_more_data = true;
        ready_for_reading.notify_one();
    }

    io_buffer_pool() = default;
    io_buffer_pool(const io_buffer_pool&) = delete;
    io_buffer_pool(io_buffer_pool&&) = delete;
    io_buffer_pool& operator=(const io_buffer_pool&) = delete;
    io_buffer_pool& operator=(io_buffer_pool&&) = delete;
};

//inline io_buffer_pool::buffer_size = 1024;

using io_buffer_pool_collection = std::vector<std::unique_ptr<io_buffer_pool>>;

#endif // IO_BUFFERS_H
