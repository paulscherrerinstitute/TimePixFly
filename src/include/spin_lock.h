#ifndef SPIN_LOCK_H
#define SPIN_LOCK_H

/*!
\file
Provide a simple spin lock
*/

#include <atomic>
#include <chrono>
#include <thread>

/*!
\brief Lock handler for a spin lock
*/
struct spin_lock final {
    using type = std::atomic_flag;  //!< Spin lock type
    static constexpr int init = 0;  //!< Initial spin lock value

    /*!
    \brief Lock the spin lock
    \param flag Reference to the lock
    */
    inline spin_lock(type& flag) noexcept
        : fl{flag}
    {
        using namespace std::chrono_literals;   
        static constexpr int spin_count = 8;
        static constexpr int yield_count = 128;
        int i;
        do {
            for (i=0; (i < spin_count) && fl.test_and_set(std::memory_order_acquire); i++);
            if (i < spin_count)
                return;
            for (i=0; (i < yield_count) && fl.test_and_set(std::memory_order_acquire); i++)
                std::this_thread::yield();
            if (i < yield_count)
                return;
            std::this_thread::sleep_for(3ns);
        } while (true);
    }

    /*!
    \brief Release the spin lock
    */
    inline ~spin_lock() noexcept
    {   
        fl.clear(std::memory_order_release);
    }

    spin_lock(const spin_lock&) = delete;
    spin_lock(spin_lock&&) = delete;
    spin_lock& operator=(const spin_lock&) = delete;
    spin_lock& operator=(spin_lock&&) = delete;

private:
    type& fl;   //!< Reference to the lock
};

#endif // SPIN_LOCK_H
