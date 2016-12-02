/* Copyright (c) 2013-2016. The SimGrid Team. All rights reserved.         */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/dynar.h>

#include <simgrid/s4u/host.hpp>

#include "src/kernel/routing/AsVivaldi.hpp"
#include "src/surf/network_interface.hpp"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_route_vivaldi, surf, "Routing part of surf");

namespace simgrid {
namespace kernel {
namespace routing {
namespace vivaldi {
simgrid::xbt::Extension<s4u::Host, Coords> Coords::EXTENSION_ID;

Coords::Coords(s4u::Host* host, const char* coordStr)
{
  if (!Coords::EXTENSION_ID.valid()) {
    Coords::EXTENSION_ID = s4u::Host::extension_create<Coords>();
  }

  unsigned int cursor;
  char* str;

  xbt_dynar_t ctn_str = xbt_str_split_str(coordStr, " ");
  xbt_assert(xbt_dynar_length(ctn_str) == 3, "Coordinates of %s must have 3 dimensions", host->name().c_str());

  this->coords = xbt_dynar_new(sizeof(double), nullptr);
  xbt_dynar_foreach (ctn_str, cursor, str) {
    double val = xbt_str_parse_double(str, "Invalid coordinate: %s");
    xbt_dynar_push(this->coords, &val);
  }
  xbt_dynar_free(&ctn_str);
  xbt_dynar_shrink(this->coords, 0);
  host->extension_set<Coords>(this);
}
Coords::~Coords()
{
  xbt_dynar_free(&coords);
}
}; // namespace vivaldi

static inline double euclidean_dist_comp(int index, xbt_dynar_t src, xbt_dynar_t dst)
{
  double src_coord = xbt_dynar_get_as(src, index, double);
  double dst_coord = xbt_dynar_get_as(dst, index, double);

  return (src_coord - dst_coord) * (src_coord - dst_coord);
  }

  static xbt_dynar_t getCoordsFromNetcard(NetCard *nc)
  {
    xbt_dynar_t res = nullptr;
    char *tmp_name;

    if (nc->isHost()) {
      tmp_name                 = bprintf("peer_%s", nc->name().c_str());
      simgrid::s4u::Host *host = simgrid::s4u::Host::by_name_or_null(tmp_name);
      if (host == nullptr)
        host = simgrid::s4u::Host::by_name_or_null(nc->name());
      if (host != nullptr)
        res = host->extension<simgrid::kernel::routing::vivaldi::Coords>()->coords;
    }
    else if(nc->isRouter() || nc->isAS()){
      tmp_name = bprintf("router_%s", nc->name().c_str());
      res = (xbt_dynar_t) xbt_lib_get_or_null(as_router_lib, tmp_name, COORD_ASR_LEVEL);
    }
    else{
      THROW_IMPOSSIBLE;
    }

    xbt_assert(res,"No coordinate found for element '%s'",tmp_name);
    free(tmp_name);

    return res;
  }
  AsVivaldi::AsVivaldi(As* father, const char* name) : AsCluster(father, name)
  {}

  void AsVivaldi::getLocalRoute(NetCard* src, NetCard* dst, sg_platf_route_cbarg_t route, double* lat)
  {
    XBT_DEBUG("vivaldi getLocalRoute from '%s'[%d] '%s'[%d]", src->name().c_str(), src->id(), dst->name().c_str(),
              dst->id());

    if (src->isAS()) {
      char* srcName = bprintf("router_%s", src->name().c_str());
      char* dstName = bprintf("router_%s", dst->name().c_str());
      route->gw_src = (sg_netcard_t)xbt_lib_get_or_null(as_router_lib, srcName, ROUTING_ASR_LEVEL);
      route->gw_dst = (sg_netcard_t)xbt_lib_get_or_null(as_router_lib, dstName, ROUTING_ASR_LEVEL);
      xbt_free(srcName);
      xbt_free(dstName);
    }

    /* Retrieve the private links */
    if (privateLinks_.size() > src->id()) {
      s_surf_parsing_link_up_down_t info = privateLinks_.at(src->id());
      if (info.linkUp) {
        route->link_list->push_back(info.linkUp);
        if (lat)
          *lat += info.linkUp->latency();
      }
    }
    if (privateLinks_.size() > dst->id()) {
      s_surf_parsing_link_up_down_t info = privateLinks_.at(dst->id());
      if (info.linkDown) {
        route->link_list->push_back(info.linkDown);
        if (lat)
          *lat += info.linkDown->latency();
      }
    }

    /* Compute the extra latency due to the euclidean distance if needed */
    if (lat) {
      xbt_dynar_t srcCoords = getCoordsFromNetcard(src);
      xbt_dynar_t dstCoords = getCoordsFromNetcard(dst);

      double euclidean_dist =
          sqrt(euclidean_dist_comp(0, srcCoords, dstCoords) + euclidean_dist_comp(1, srcCoords, dstCoords)) +
          fabs(xbt_dynar_get_as(srcCoords, 2, double)) + fabs(xbt_dynar_get_as(dstCoords, 2, double));

      XBT_DEBUG("Updating latency %f += %f", *lat, euclidean_dist);
      *lat += euclidean_dist / 1000.0; // From .ms to .s
    }
}

}}}
