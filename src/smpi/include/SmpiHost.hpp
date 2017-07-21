/* Copyright (c) 2017. The SimGrid Team. All rights reserved.               */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SMPI_HOST_HPP_
#define SMPI_HOST_HPP_

#include "src/include/smpi/smpi_utils.hpp"

#include "simgrid/s4u/Host.hpp"
#include <string>
#include <vector>
#include <xbt/Extendable.hpp>
#include <xbt/config.hpp>

namespace simgrid {
namespace smpi {

void sg_smpi_host_init();
static void onHostDestruction(simgrid::s4u::Host& host);
static void onCreation(simgrid::s4u::Host& host);

class SmpiHost {

  private:
  std::vector<s_smpi_factor_t> orecv_parsed_values;
  std::vector<s_smpi_factor_t> osend_parsed_values;
  std::vector<s_smpi_factor_t> oisend_parsed_values;
  simgrid::s4u::Host *host = nullptr;

  public:
  static simgrid::xbt::Extension<simgrid::s4u::Host, SmpiHost> EXTENSION_ID;

  explicit SmpiHost(simgrid::s4u::Host *ptr);
  ~SmpiHost();

  double orecv(size_t size);
  double osend(size_t size);
  double oisend(size_t size);
};

}
}
#endif
