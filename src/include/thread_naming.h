#pragma once

#ifndef THREAD_NAMING_H
#define THREAD_NAMING_H

/*!
\file
Provide means to name a thread
*/

#include <string>
#include <thread>
#include <pthread.h>

#ifdef __linux__
/*!
\brief Set thread name of current thread
\param name New thread name
*/
inline void set_thread_name(const std::string& name)
{
    auto tid = pthread_self();
    pthread_setname_np(tid, name.c_str());
}
#else
/*!
\brief Dummy function
Only Linux supports `pthread_setname_np()`
\param name New thread name
*/
inline void set_thread_name(const std::string& name)
{
    ; // only linux supports pthread_setname_np()
}
#endif

#endif // THREAD_NAMING_H
