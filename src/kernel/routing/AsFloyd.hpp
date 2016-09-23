/* Copyright (c) 2013-2016. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SURF_ROUTING_FLOYD_HPP_
#define SURF_ROUTING_FLOYD_HPP_

#include "src/kernel/routing/AsRoutedGraph.hpp"

namespace simgrid {
namespace kernel {
namespace routing {

/** Floyd routing data: slow initialization, fast lookup, lesser memory requirements, shortest path routing only */
class XBT_PRIVATE AsFloyd: public AsRoutedGraph {
public:
  explicit AsFloyd(const char *name);
  ~AsFloyd() override;

  void getRouteAndLatency(NetCard *src, NetCard *dst, sg_platf_route_cbarg_t into, double *latency) override;
  void addRoute(sg_platf_route_cbarg_t route) override;
  void seal() override;

private:
  /* vars to compute the Floyd algorithm. */
  int *predecessorTable_;
  double *costTable_;
  sg_platf_route_cbarg_t *linkTable_;
};

}}} // namespaces

#endif /* SURF_ROUTING_FLOYD_HPP_ */
