#pragma once

/*!
\file
Provide argument parsing and functions to pin a thread to a cpu virtual core
*/

#ifndef CPU_MASK_H
#define CPU_MASK_H

#include <cerrno>
#include <cassert>
#include <vector>
#include <algorithm>
#include <string>
#include <cstring>
#include <functional>
#include <pthread.h>

/*!
\brief Namespace for cpu_mask related functionality
*/
namespace cpu_mask {

    /*!
    \brief Get thread system ID
    \return Thread system ID
    */
    inline pthread_t get_tid() noexcept
    {
        return pthread_self();
    }

    /*!
    \brief Set cpu virtual core affinity mask
    \param tid Thread system ID from \ref get_tid()
    \param cpu Cpu virtual core, none for negative values
    \return 0 if successful, system error number otherwise
    */
    inline int set_affinity(pthread_t tid, int cpu) noexcept
    {
        if (cpu < 0)
            return 0;

        if ((size_t)cpu >= sizeof(cpu_set_t))
            return EINVAL;

        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(cpu, &cpu_set);

        return pthread_setaffinity_np(tid, sizeof(cpu_set), &cpu_set);
    }

    /*!
    \brief System error description
    \param err System error code
    \return Description of system error
    */
    inline std::string error(int err) noexcept
    {
        return strerror(err);
    }

    /*!
    \brief CPU affinity settings
    */
    struct cpu_mask_t final {
        int writer_cpu = -1;                //!< Writer thread CPU
        int reader_cpu = -1;                //!< Reader thread CPU
        std::vector<int> analysis_cpus;     //!< CPUs for the Analysis threads

        /*!
        \brief Get analysis thread CPU
        \param threadNo Analysis thread no
        \return Analysis thread CPU, or -1 if none is set
        */
        inline int get_cpu(unsigned threadNo) const noexcept
        {
            return (threadNo < analysis_cpus.size()) ? analysis_cpus[threadNo] : -1;
        }

        /*!
        \brief Clear affinity settings
        */
        inline void clear() noexcept
        {
            writer_cpu = reader_cpu = -1;
            analysis_cpus.clear();
        }
    };

    /*!
    \brief Parse CPU affinity setting
    \param mask Update this mask with the new settings
    \param str Affinity settings
        affinity = (a|r|w):cpu-set | affinity;affinity
        cpu-set = cpu | range | cpu-set,cpu-set
        range = cpu-cpu
    \param fn Error handler function with arguments error position and message
    */
    inline void parse(cpu_mask_t& mask, const std::string& str, const std::function<void(unsigned, const std::string&)>& fn)
    {
        unsigned next = 0;
        int id = -1;
        int range_start = -1;
        int number = -1;

        auto mask_set = [&mask, &id, &range_start, &number]() -> bool {
            switch (id) {
                case 'w': {
                    if (mask.writer_cpu >= 0)
                        return false;
                    if (range_start >= 0)
                        return false;
                    mask.writer_cpu = number;
                    number = -1;
                    return true;
                }
                case 'r': {
                    if (mask.reader_cpu >= 0)
                        return false;
                    if (range_start >= 0)
                        return false;
                    mask.reader_cpu = number;
                    number = -1;
                    return true;
                }
                case 'a': {
                    auto& vec = mask.analysis_cpus;
                    if (range_start < 0)
                        range_start = number;
                    for (int i=range_start; i<=number; i++)
                        vec.push_back(i);
                    range_start = -1;
                    number = -1;
                    return true;
                }
            }
            return false;
        };

        auto token = [&next, &str]() -> char {
            if (str.size() > next)
                return str[next++];
            return 0;
        };

        enum { ID=0, COLON=1, NUM=2, END=3 } next_state = ID;
        do {
            char ch = token();
            switch (next_state) {
                case ID: {
                    switch (ch) {
                        case 'a': {
                            if (!mask.analysis_cpus.empty())
                                return fn(next, "analysis affinity already set");
                            [[fallthrough]];
                        }
                        case 'r':
                        case 'w': {
                            id = ch;
                            next_state = COLON;
                            break;
                        }
                        default:
                            return fn(next, "one of the characters a,r,w expected");
                    }
                    break;
                }
                case COLON: {
                    if (ch != ':')
                        return fn(next, "colon expected");
                    next_state = NUM;
                    break;
                }
                case NUM: {
                    switch (ch) {
                        case 0:
                        case ';': {
                            if (number < 0)
                                return fn(next, "expected number");
                            if (!mask_set())
                                return fn(next, "affinity can only be set once");
                            if (ch == 0)
                                next_state = END;
                            else
                                next_state = ID;
                            break;
                        }
                        case '-': {
                            if (number < 0)
                                return fn(next, "expected number");
                            range_start = number;
                            number = -1;
                            break;
                        }
                        case ',': {
                            if (number < 0)
                                return fn(next, "expected number");
                            if (!mask_set())
                                return fn(next, "affinity can only be set once to an exclusive number");
                            break;
                        }
                        case '0':
                        case '1':
                        case '2':
                        case '3':
                        case '4':
                        case '5':
                        case '6':
                        case '7':
                        case '8':
                        case '9': {
                            if (number < 0)
                                number = 0;
                            number = 10 * number + (ch - '0');
                        }
                    }
                }
                case END:
                    break;
                default:
                    return fn(next, "internal error (undefined state)");
            }
        } while (next_state != END);
    }

} // namespace cpu_mask

#endif
