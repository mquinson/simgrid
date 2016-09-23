/* Copyright (c) 2007-2016. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_KERNEL_ACTIVITY_ACTIVITYIMPL_HPP
#define SIMGRID_KERNEL_ACTIVITY_ACTIVITYIMPL_HPP

#include <string>
#include <list>

#include <xbt/base.h>
#include "simgrid/forward.h"

#include <simgrid/simix.hpp>

namespace simgrid {
namespace kernel {
namespace activity {

  XBT_PUBLIC_CLASS ActivityImpl {
  public:
    ActivityImpl();
    virtual ~ActivityImpl();
    e_smx_state_t state = SIMIX_WAITING; /* State of the activity */
    std::string name;                    /* Activity name if any */
    std::list<smx_simcall_t> simcalls;   /* List of simcalls waiting for this activity */

    virtual void suspend()=0;
    virtual void resume()=0;
    virtual void post() =0; // What to do when a simcall terminates

    void ref();
    void unref();
  private:
    int refcount = 1;
  };
}}} // namespace simgrid::kernel::activity

#endif /* SIMGRID_KERNEL_ACTIVITY_ACTIVITYIMPL_HPP */
