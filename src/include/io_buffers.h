#ifndef IO_BUFFERS_H
#define IO_BUFFERS_H

/*!
\file
Code for buffering incoming IO
*/

#include <atomic>
#include <vector>
#include <map>
#include <list>
#include <mutex>
#include <condition_variable>

/*!
\brief Buffer for holding partial raw stream chunk data
*/
struct io_buffer final {
    inline static std::atomic<unsigned> next_id;    //!< Buffer id for next buffer
    std::vector<char> content;                      //!< Content of this buffer
    size_t content_offset = 0;                      //!< Content offset within raw event data packet chunk
    size_t content_size = 0;                        //!< Content size in number of bytes
    size_t chunk_size = 0;                          //!< Raw data event packet chunk size in number of bytes
    unsigned id;                                    //!< Id of this buffer

    /*!
    \brief Constructor
    \param sz IO buffer size in bytes
    */
    io_buffer(size_t sz)
        : content(sz), id{next_id.fetch_add(1)}
    {}

    io_buffer(const io_buffer&) = delete;
    io_buffer(io_buffer&&) = default;                   //!< Move constructor
    io_buffer& operator=(const io_buffer&) = delete;
    io_buffer& operator=(io_buffer&&) = default;        //!< Move assignment \return Reference to `this`
};

/*!
\brief Pool of IO buffers
*/
struct io_buffer_pool final {
    inline static size_t buffer_size = 1024;        //!< Default IO buffer size in bytes

    /*!
    \brief Buffer pool content data type

    The buffer pool content is a multimap with sorted keys referring to the raw event data packet chunk number.
    The raw event data packet chunks are split into pieces of `buffer_size`that end up in the multimap with
    the their associated chunk number.

    Since keys are sorted, the begin of the multimap contains the first seen piece of the oldest chunk in the pool.
    */
    using buffer_type = std::multimap<uint64_t, std::unique_ptr<io_buffer>>;

    using element_type = buffer_type::value_type;   //!< Alias for multimap element type
    buffer_type buffer;                             //!< The collection of IO buffers
    std::list<std::unique_ptr<io_buffer>> free_list;//!< Empty IO buffers for reuse
    std::mutex modify_buffer;                       //!< Protect multimap with buffers
    std::condition_variable ready_for_reading;      //!< Condition: there are full buffers
    std::mutex modify_free_list;                    //!< Protect `free_list`
    bool no_more_data = false;                      //!< Flag for "no more data is coming"

    /*!
    \brief Get a buffer with some valid content

    Block on `ready_for_reading` condvar if there are no IO buffers (`buffer` is empty) and
    more data is expected (`no_more_data` is false).

    \return Pair of (chunk number, buffer pointer). If no data is coming, the buffer pointer is the nullptr.
    */
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

    /*!
    \brief Put a used buffer back to the `free_list`
    \param buf Will be moved into the `free_list`
    */
    inline void put_empty_buffer(std::unique_ptr<io_buffer>&& buf)
    {
        std::lock_guard lock(modify_free_list);
        free_list.push_front(std::move(buf));
    }

    /*!
    \brief Get an empty buffer from the `free_list`, or create a new one
    \return Smart pointer to empty IO buffer ready for filling up
    */
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

    /*!
    \brief Insert full buffer into the multimap of full buffers
    \param element Will be moved into the multimap
    */
    inline void put_nonempty_buffer(element_type&& element)
    {
        std::lock_guard lock(modify_buffer);
        buffer.insert(std::move(element));
        ready_for_reading.notify_one();
    }

    /*!
    \param Signal that no more data is coming
    */
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

/*!
\brief Collection of IO buffer pools
There's one buffer per detector chip.
*/
using io_buffer_pool_collection = std::vector<std::unique_ptr<io_buffer_pool>>;

#endif // IO_BUFFERS_H
