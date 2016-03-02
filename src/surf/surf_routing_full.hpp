/* Copyright (c) 2013-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SURF_ROUTING_FULL_HPP_
#define SURF_ROUTING_FULL_HPP_

#include <xbt/base.h>

#include "surf_routing_RoutedGraph.hpp"

namespace simgrid {
namespace surf {

/***********
 * Classes *
 ***********/
class XBT_PRIVATE AsFull;

/** Full routing: fast, large memory requirements, fully expressive */
class AsFull: public AsRoutedGraph {
public:

  AsFull(const char*name);
  void Seal() override;
  ~AsFull();

  void getRouteAndLatency(NetCard *src, NetCard *dst, sg_platf_route_cbarg_t into, double *latency) override;
  void addRoute(sg_platf_route_cbarg_t route) override;

  sg_platf_route_cbarg_t *routingTable_ = nullptr;
};

}
}

#endif /* SURF_ROUTING_FULL_HPP_ */
