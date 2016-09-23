/* Copyright (c) 2007-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_SAFETY_H
#define SIMGRID_MC_SAFETY_H

#include <stdint.h>

#include <memory>

#include <simgrid_config.h>

#include <xbt/base.h>

#include "src/mc/mc_forward.hpp"
#include "src/mc/mc_state.h"

namespace simgrid {
namespace mc {

enum class ReductionMode {
  unset,
  none,
  dpor,
};

extern XBT_PRIVATE simgrid::mc::ReductionMode reduction_mode;

}
}

#endif
