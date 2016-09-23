/* Copyright (c) 2013-2016. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SURF_ROUTING_VIVALDI_HPP_
#define SURF_ROUTING_VIVALDI_HPP_

#include "src/kernel/routing/AsCluster.hpp"

namespace simgrid {
namespace kernel {
namespace routing {

/* This derivates from cluster because each host has a private link */
class XBT_PRIVATE AsVivaldi: public AsCluster {
public:
  explicit AsVivaldi(const char *name);
  ~AsVivaldi() override;

  xbt_dynar_t getOneLinkRoutes() override {return nullptr;};
  void getRouteAndLatency(NetCard *src, NetCard *dst, sg_platf_route_cbarg_t into, double *latency) override;
};

}}} // namespace

#endif /* SURF_ROUTING_VIVALDI_HPP_ */
