/* Copyright (c) 2013-2017. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <boost/algorithm/string.hpp>

#include "simgrid/s4u/Engine.hpp"
#include "simgrid/s4u/Host.hpp"

#include "src/kernel/routing/NetPoint.hpp"
#include "src/kernel/routing/VivaldiZone.hpp"
#include "src/surf/network_interface.hpp"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_route_vivaldi, surf, "Routing part of surf");

namespace simgrid {
namespace kernel {
namespace routing {
namespace vivaldi {
simgrid::xbt::Extension<NetPoint, Coords> Coords::EXTENSION_ID;

Coords::Coords(NetPoint* netpoint, const char* coordStr)
{
  if (not Coords::EXTENSION_ID.valid())
    Coords::EXTENSION_ID = NetPoint::extension_create<Coords>();

  std::vector<std::string> string_values;
  boost::split(string_values, coordStr, boost::is_any_of(" "));
  xbt_assert(string_values.size() == 3, "Coordinates of %s must have 3 dimensions", netpoint->cname());

  for (auto str : string_values)
    coords.push_back(xbt_str_parse_double(str.c_str(), "Invalid coordinate: %s"));
  coords.shrink_to_fit();

  netpoint->extension_set<Coords>(this);
  XBT_DEBUG("Coords of %s %p: %s", netpoint->cname(), netpoint, coordStr);
}
Coords::~Coords() = default;
}; // namespace vivaldi

static inline double euclidean_dist_comp(int index, std::vector<double>* src, std::vector<double>* dst)
{
  double src_coord = src->at(index);
  double dst_coord = dst->at(index);

  return (src_coord - dst_coord) * (src_coord - dst_coord);
}

static std::vector<double>* getCoordsFromNetpoint(NetPoint* np)
{
  simgrid::kernel::routing::vivaldi::Coords* coords = np->extension<simgrid::kernel::routing::vivaldi::Coords>();
  xbt_assert(coords, "Please specify the Vivaldi coordinates of %s %s (%p)",
             (np->isNetZone() ? "Netzone" : (np->isHost() ? "Host" : "Router")), np->cname(), np);
  return &coords->coords;
}
VivaldiZone::VivaldiZone(NetZone* father, const char* name) : ClusterZone(father, name)
{
}

void VivaldiZone::setPeerLink(NetPoint* netpoint, double bw_in, double bw_out, const char* coord)
{
  xbt_assert(netpoint->netzone() == this, "Cannot add a peer link to a netpoint that is not in this netzone");

  new simgrid::kernel::routing::vivaldi::Coords(netpoint, coord);

  std::string link_up   = "link_" + netpoint->name() + "_UP";
  std::string link_down = "link_" + netpoint->name() + "_DOWN";
  surf::LinkImpl* linkUp   = surf_network_model->createLink(link_up.c_str(), bw_out, 0, SURF_LINK_SHARED);
  surf::LinkImpl* linkDown = surf_network_model->createLink(link_down.c_str(), bw_in, 0, SURF_LINK_SHARED);
  privateLinks_.insert({netpoint->id(), {linkUp, linkDown}});
}

void VivaldiZone::getLocalRoute(NetPoint* src, NetPoint* dst, sg_platf_route_cbarg_t route, double* lat)
{
  XBT_DEBUG("vivaldi getLocalRoute from '%s'[%d] '%s'[%d]", src->cname(), src->id(), dst->cname(), dst->id());

  if (src->isNetZone()) {
    std::string srcName = "router_" + src->name();
    std::string dstName = "router_" + dst->name();
    route->gw_src       = simgrid::s4u::Engine::getInstance()->getNetpointByNameOrNull(srcName.c_str());
    route->gw_dst       = simgrid::s4u::Engine::getInstance()->getNetpointByNameOrNull(dstName.c_str());
  }

  /* Retrieve the private links */
  if (privateLinks_.find(src->id()) != privateLinks_.end()) {
    std::pair<surf::LinkImpl*, surf::LinkImpl*> info = privateLinks_.at(src->id());
    if (info.first) {
      route->link_list->push_back(info.first);
      if (lat)
        *lat += info.first->latency();
    }
  }
  if (privateLinks_.find(dst->id()) != privateLinks_.end()) {
    std::pair<surf::LinkImpl*, surf::LinkImpl*> info = privateLinks_.at(dst->id());
    if (info.second) {
      route->link_list->push_back(info.second);
      if (lat)
        *lat += info.second->latency();
    }
  }

  /* Compute the extra latency due to the euclidean distance if needed */
  if (lat) {
    std::vector<double>* srcCoords = getCoordsFromNetpoint(src);
    std::vector<double>* dstCoords = getCoordsFromNetpoint(dst);

    double euclidean_dist =
        sqrt(euclidean_dist_comp(0, srcCoords, dstCoords) + euclidean_dist_comp(1, srcCoords, dstCoords)) +
        fabs(srcCoords->at(2)) + fabs(dstCoords->at(2));

    XBT_DEBUG("Updating latency %f += %f", *lat, euclidean_dist);
    *lat += euclidean_dist / 1000.0; // From .ms to .s
  }
}
}
}
}
