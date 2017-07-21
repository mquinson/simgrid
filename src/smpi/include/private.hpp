/* Copyright (c) 2016. The SimGrid Team. All rights reserved.               */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SMPI_PRIVATE_HPP
#define SMPI_PRIVATE_HPP

#include "src/instr/instr_smpi.h"
#include <unordered_map>
#include <vector>
#include "src/internal_config.h"

/**
 * Get the address of the beginning of the memory page where addr is located.
 * Note that we use an integer division here, so (a/b)*b is not a, unless a%b == 0
 *
 * This is used when privatizing.
 */
#define TOPAGE(addr) (void *)(((unsigned long)(addr) / xbt_pagesize) * xbt_pagesize)

#if HAVE_PAPI
typedef
    std::vector<std::pair</* counter name */std::string, /* counter value */long long>> papi_counter_t;
XBT_PRIVATE papi_counter_t& smpi_process_papi_counters();
XBT_PRIVATE int smpi_process_papi_event_set();
#endif

extern std::unordered_map<std::string, double> location2speedup;

/** @brief Returns the last call location (filename, linenumber). Process-specific. */
extern "C" {
XBT_PUBLIC(smpi_trace_call_location_t*) smpi_process_get_call_location();
XBT_PUBLIC(smpi_trace_call_location_t*) smpi_trace_get_call_location();
}

typedef enum {
  SMPI_PRIVATIZE_NONE    = 0,
  SMPI_PRIVATIZE_MMAP    = 1,
  SMPI_PRIVATIZE_DLOPEN  = 2,
  SMPI_PRIVATIZE_DEFAULT = SMPI_PRIVATIZE_MMAP
} smpi_priv_strategies;

extern XBT_PRIVATE int smpi_privatize_global_variables;

#endif

