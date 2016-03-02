/* Copyright (c) 2013-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/base.h>

#include "surf_routing.hpp"
#include "simgrid/s4u/as.hpp"


#ifndef SURF_ROUTING_NONE_HPP_
#define SURF_ROUTING_NONE_HPP_

namespace simgrid {
namespace surf {

/** No specific routing. Mainly useful with the constant network model */
class XBT_PRIVATE AsNone : public s4u::As {
public:
  AsNone(const char*name);
  ~AsNone();

  void getRouteAndLatency(NetCard *src, NetCard *dst, sg_platf_route_cbarg_t into, double *latency) override;
  void getGraph(xbt_graph_t graph, xbt_dict_t nodes, xbt_dict_t edges) override;
};

}
}

#endif /* SURF_ROUTING_NONE_HPP_ */
