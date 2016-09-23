/* Copyright (c) 2009-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

/* \file ThreadContext.hpp Context switching with native threads */

#ifndef SIMGRID_SIMIX_THREAD_CONTEXT_HPP
#define SIMGRID_SIMIX_THREAD_CONTEXT_HPP

#include <simgrid/simix.hpp>


namespace simgrid {
namespace kernel {
namespace context {

class ThreadContext;
class ThreadContextFactory;

class ThreadContext : public AttachContext {
public:
  friend ThreadContextFactory;
  ThreadContext(std::function<void()> code,
          void_pfn_smxprocess_t cleanup_func,
          smx_actor_t process, bool maestro =false);
  ~ThreadContext() override;
  void stop() override;
  void suspend() override;
  void attach_start() override;
  void attach_stop() override;
private:
  /** A portable thread */
  xbt_os_thread_t thread_ = nullptr;
  /** Semaphore used to schedule/yield the process */
  xbt_os_sem_t begin_ = nullptr;
  /** Semaphore used to schedule/unschedule */
  xbt_os_sem_t end_ = nullptr;
private:
  static void* wrapper(void *param);
  static void* maestro_wrapper(void *param);
public:
  void start();
};

class ThreadContextFactory : public ContextFactory {
public:
  ThreadContextFactory();
  ~ThreadContextFactory() override;
  ThreadContext* create_context(std::function<void()> code,
    void_pfn_smxprocess_t cleanup_func,  smx_actor_t process) override;
  void run_all() override;
  ThreadContext* self() override;

  // Optional methods:
  ThreadContext* attach(void_pfn_smxprocess_t cleanup_func, smx_actor_t process) override;
  ThreadContext* create_maestro(std::function<void()> code, smx_actor_t process) override;
};

}}} // namespace

#endif
