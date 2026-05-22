#pragma once

#ifndef THREAD_SIGNAL_H
#define THREAD_SIGNAL_H

/*!
\file
Provide inter thread signals
*/

#include <condition_variable>
#include <mutex>
#include <atomic>
#include <vector>
#include <climits>
#include <cassert>

/*!
\brief Thread signalling functionality
*/
namespace thread_signal {

    template<bool type> class multi;

    /*!
    \brief Get the bit size of a value
    \param value Value of interest
    \return Size of value in bits
    */
    template <typename T>
    constexpr unsigned bit_size(const T& value) noexcept
    {
        return sizeof(value) * CHAR_BIT;
    }

    static constexpr bool with_shutdown = true;         //!< Single signal wait can be interrupted by shutdown
    static constexpr bool no_shutdown = false;          //!< Single signal wait uninterruptible
    static constexpr bool send = true;                  //!< Multi signal where several threads must send the signal
    static constexpr bool reset_with_shutdown = false;  //!< Multi signal where several threads must reset the signal, with shutdown interruptible wait

    /*!
    \brief Signal base
    */
    class base {
      protected:
        std::condition_variable cond;   //!< Signal sent condition
        std::mutex lock;                //!< Potect signal

      public:
        /*!
        \brief Notify one waiter after condition is fullfilled
        */
        inline void notify_one()
        {
            cond.notify_one();
        }

        /*!
        \brief Notify all waiters after condition is fullfilled
        */
        inline void notify_all()
        {
            cond.notify_all();
        }
    };

    /*!
    \brief Signal from single thread
    \tparam kind With, or no shutdown
    */
    template<bool kind=no_shutdown>
    class single final : base {
        std::vector<base*> dep;             //!< Dependent signals
        std::atomic_bool signal = false;    //!< Signal flag

    public:
        /*!
        \brief Construct single<no_shutdown> signal
        */
        inline single() noexcept
        {
            dep.push_back(this);
        }

        /*!
        \brief Send signal

        Wait for threads that read the signal before reset.
        */
        inline void send() noexcept
        {
            signal = true;
            for (auto* sig : dep)
                sig->notify_all();
        }
        
        /*!
        \brief Wait for and reset the signal

        Wait for threads that read the signal before reset.
        */
        inline void wait_reset() noexcept
        {
            std::unique_lock lck{lock};
            while (!signal)
                cond.wait(lck);
            signal = false;
        }

        /*!
        \brief Wait for signal
        */
        inline void wait() noexcept
        {
            std::unique_lock lck{lock};
            while (!signal)
                cond.wait(lck);
        }

        /*!
        \brief Reset signal

        Wait for threads that read the signal before reset.
        */
        inline void reset() noexcept
        {
            signal = false;
        }

        friend single<with_shutdown>;                       //!< This may access the signal flag
        friend multi<thread_signal::reset_with_shutdown>;   //!< This may access the signal flag
    };

    /*!
    \brief Signal from single thread
    */
    template<>
    class single<true> final : base {
        single<false>& shutdown;        //!< Shutdown signal
        bool signal = false;            //!< Signal flag

    public:
        /*!
        \brief Signal from single thread

        This registers the signal as depending on the shutdown signal.
        \param shutdown_signal 
        */
        inline explicit single(single<false>& shutdown_signal) noexcept
            : shutdown{shutdown_signal}
        {
            shutdown.dep.push_back(this);
        }

        /*!
        \brief Send signal

        Wait for thread that reads signal before reset.
        */
        inline void send() noexcept
        {
            std::lock_guard lck{lock};
            signal = true;
            notify_all();
        }
        
        /*!
        \brief Wait for and reset the signal

        Wait for threads that read the signal before reset.
        \return True if shutdown signal was sent
        */
        inline bool wait_reset() noexcept
        {
            std::unique_lock lck{lock};
            bool ss;
            {
                while (!signal && !(ss = shutdown.signal))
                    cond.wait(lck);
            }
            signal = false;
            return ss;
        }

        /*!
        \brief Wait for signal
        \return True if shutdown signal was sent
        */
        inline bool wait() noexcept
        {
            std::unique_lock lck{lock};
            bool ss;
            while (!signal && !(ss = shutdown.signal))
                cond.wait(lck);
            return ss;
        }

        /*!
        \brief Reset signal

        Wait for thread that reads signal before reset.
        */
        inline void reset() noexcept
        {
            signal = false;
        }
    };

    /*!
    \brief Signal from/for multiple threads
    \tparam kind Either ultiple sends or multiple resets
    */
    template<bool kind=send>
    class multi final : base {
        const unsigned nthreads;            //!< Number of threads
        std::atomic<unsigned> sendcnt = 0u; //!< Threads that already have sent the signal
        bool signal = false;                //!< Signal flag

    public:
        /*!
        \brief Construct signal
        \param num_threads For this many threads
        */
        inline explicit multi(unsigned num_threads) noexcept
            : nthreads{num_threads}
        {}

        /*!
        \brief Send signal

        This notifies waiters, if all threads have sent the signal.
        Wait for threads that read signal before reset.
        */
        inline void send() noexcept
        {
            if (++sendcnt == nthreads) {
                std::lock_guard lck{lock};
                signal = true;
                notify_all();
            }
        }

        /*!
        \brief Wait for and reset signal

        Wait for threads that read the signal before reset.
        */
        inline void wait_reset() noexcept
        {
            std::unique_lock lck{lock};
            while (! signal)
                cond.wait(lck);
            signal = false;
            sendcnt = 0u;
        }

        /*!
        \brief Wait for signal
        */
        inline void wait() noexcept
        {
            std::unique_lock lck{lock};
            while (! signal)
                cond.wait(lck);
        }

        /*!
        \brief Reset signal

        Wait for threads that read signal before reset.
        */
        inline void reset() noexcept
        {
            signal = false;
            sendcnt = 0u;
        }
    };

    /*!
    \brief Signal with ultiple threads resetting it.
    */
    template<>
    class multi<reset_with_shutdown> final : base {
        single<false>& shutdown;        //!< Shutdown signal
        const unsigned nthreads;        //!< Number of threads
        unsigned signalbits = 0u;       //!< One bit per thread as signal

    public:
        /*!
        \brief Construct multi-reset signal
        \param shutdown_signal  Signal that may interrupt waiters for this signal
        \param num_threads      Number of threads
        */
        inline explicit multi(single<false>& shutdown_signal, unsigned num_threads) noexcept
            : shutdown{shutdown_signal}, nthreads{num_threads}
        {
            assert(num_threads <= bit_size(signalbits));
            shutdown.dep.push_back(this);
        }

        /*!
        \brief Send signal
        */
        inline void send() noexcept
        {
            std::lock_guard lck{lock};
            signalbits = (1ul << nthreads) - 1ul;
            notify_all();
        }

        /*!
        \brief Wait on and reset signal

        Wait for threads reading the signal before reset.
        \param thread Which thread
        \return True if shutdown signal interrupted the wait
        */
        inline bool wait_reset(unsigned thread) noexcept
        {
            auto mask = 1u << thread;
            bool ss;
            std::unique_lock lck{lock};
            while (! (signalbits & mask) && !(ss = shutdown.signal))
                cond.wait(lck);
            signalbits ^= mask;
            return ss;
        }

        /*!
        \brief Wait for signal
        \param thread Which thread
        \return True if shutdown signal interrupted the wait
        */
        inline bool wait(unsigned thread) noexcept
        {
            auto mask = 1u << thread;
            bool ss;
            std::unique_lock lck{lock};
            while (! (signalbits & mask) && !(ss = shutdown.signal))
                cond.wait(lck);
            return ss;
        }

        /*!
        \brief Reset signal

        Wait for threads reading the signal before reset.
        \param thread Which thread
        */
        inline void reset(unsigned thread) noexcept
        {
            auto mask = 1u << thread;
            signalbits &= ~mask;
        }
    };
} // namespace signal

#endif // THREAD_SGINLA_H