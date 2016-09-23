/* Copyright (c) 2006-2015. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_S4U_MUTEX_HPP
#define SIMGRID_S4U_MUTEX_HPP

#include <mutex>
#include <utility>

#include <boost/intrusive_ptr.hpp>

#include <xbt/base.h>
#include "simgrid/simix.h"

namespace simgrid {
namespace s4u {

class ConditionVariable;

/** @brief A classical mutex, but blocking in the simulation world
 *
 * It is strictly impossible to use a real mutex (such as
 * [std::mutex](http://en.cppreference.com/w/cpp/thread/mutex)
 * or [pthread_mutex_t](http://pubs.opengroup.org/onlinepubs/007908775/xsh/pthread_mutex_lock.html)),
 * because it would block the whole simulation.
 * Instead, you should use the present class, that is a drop-in replacement of
 * [std::mutex](http://en.cppreference.com/w/cpp/thread/mutex).
 *
 * As for any S4U object, Mutexes are using the @ref "RAII idiom" s4u_raii for memory management.
 * Use createMutex() to get a ::MutexPtr to a newly created mutex and only manipulate ::MutexPtr.
 *
 */
XBT_PUBLIC_CLASS Mutex {
friend ConditionVariable;
private:
  friend simgrid::simix::Mutex;
  simgrid::simix::Mutex* mutex_;
  Mutex(simgrid::simix::Mutex* mutex) : mutex_(mutex) {}

  /* refcounting of the intrusive_ptr is delegated to the implementation object */
  friend void intrusive_ptr_add_ref(Mutex* mutex)
  {
    xbt_assert(mutex);
    SIMIX_mutex_ref(mutex->mutex_);
  }
  friend void intrusive_ptr_release(Mutex* mutex)
  {
    xbt_assert(mutex);
    SIMIX_mutex_unref(mutex->mutex_);
  }
public:
  using Ptr = boost::intrusive_ptr<Mutex>;

  // No copy:
  /** You cannot create a new mutex by copying an existing one. Use MutexPtr instead */
  Mutex(Mutex const&) = delete;
  /** You cannot create a new mutex by value assignment either. Use MutexPtr instead */
  Mutex& operator=(Mutex const&) = delete;

  /** Constructs a new mutex */
  static Ptr createMutex();

public:
  void lock();
  void unlock();
  bool try_lock();
};

using MutexPtr = Mutex::Ptr;

}} // namespace simgrid::s4u

#endif /* SIMGRID_S4U_MUTEX_HPP */
