/* Copyright (c) 2007-2017. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMIX_IO_PRIVATE_H
#define SIMIX_IO_PRIVATE_H

#include <xbt/base.h>

#include "simgrid/simix.h"
#include "popping_private.h"

XBT_PRIVATE smx_activity_t SIMIX_file_read(surf_file_t fd, sg_size_t size);
XBT_PRIVATE smx_activity_t SIMIX_file_write(surf_file_t fd, sg_size_t size);

XBT_PRIVATE void SIMIX_io_destroy(smx_activity_t synchro);
XBT_PRIVATE void SIMIX_io_finish(smx_activity_t synchro);

#endif
