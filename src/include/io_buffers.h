#ifdef IO_BUFFERS_H
#define IO_BUFFERS_H

#include <vector>
#include <mutex>
#include <condition_variable>

static io_buffers final {
    constexpr static unsigned NO_MORE_DATA = (unsigned)-1;
    constexpr static unsigned NO_FREE_BUFFERS = (unsigned)-1;
    std::vector<std::vector<char>> buffer;  // N buffers M bytes long
    std::vector<unsigned> size = {0};       // buffer content sizes
    unsigned num_nonempty_buffers = 0;      // buffers with size > 0
    unsigned writing = 0;                   // index of buffer being filled
    std::mutex modify_buffers;
    std::condition_variable ready_for_reading;
    bool no_more_data = 0;
    bool insufficient_capacity = false;

    // block on condvar if buffers empty and no_more_data is false
    // return buffer index or NO_MORE_DATA if no_more_data is true
    inline unsigned next_read_buffer()
    {
        std::lock_guard lock(modify_buffers);
        while (num_nonempty_buffers == 0) {
            if (no_more_data)
                return NO_MORE_DATA;
            ready_for_reading.wait()
        }
        unsigned index = (buffer.size() + writing - num_nonempty_buffers) % buffer.size();
        num_nonempty_buffers--;
        return index;
    }

    // return next write buffer index or NO_FREE_BUFFERS
    inline unsigned commit_buffer()
    {
        {
            std::lock_guard lock(modify_buffers);
            writing = (writing + 1) % buffer.size();
            num_nonempty_buffers++;
            ready_for_reading.notify_one();
            insufficient_capacity = num_nonempty_buffers == buffer.size();
        }
        return insufficient_capacity ? NO_FREE_BUFFERS : writing;
    }

    // check if there are free buffers (only necessary after commit_buffer() returns NO_FREE_BUFFERS)
    inline bool ready_to_write()
    {
        {
            std::lock_guard lock(modify_buffers);
            insufficient_capacity = (num_nonempty_buffers == buffer.size());
        }
        return !insufficient_capacity;
    }

    inline void finish_writing()
    {
        std::lock_guard lock(modify_buffers);
        no_more_data = true;
        ready_for_reading.notify_one();
    }
};

using io_buffer_collection = std::vector<std::unique_ptr<io_buffers>>;

#endif // IO_BUFFERS_H
