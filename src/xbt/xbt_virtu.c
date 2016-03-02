/* virtu - virtualization layer for XBT to choose between GRAS and MSG implementation */

/* Copyright (c) 2007-2010, 2012-2014. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "xbt/misc.h"
#include "xbt/virtu.h"
#include "xbt/function_types.h"
#include "simgrid/simix.h"

static int xbt_fake_pid(void)
{
  return 0;
}

int_f_void_t xbt_getpid = xbt_fake_pid;

const char *xbt_procname(void)
{
  return SIMIX_process_self_get_name();
}
