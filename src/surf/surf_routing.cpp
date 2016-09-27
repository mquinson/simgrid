/* Copyright (c) 2009-2011, 2013-2016. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <vector>

#include <xbt/signal.hpp>

#include <simgrid/s4u/host.hpp>

#include "surf_routing.hpp"

#include "simgrid/sg_config.h"
#include "storage_interface.hpp"

#include "src/kernel/routing/AsImpl.hpp"
#include "src/surf/xml/platf.hpp" // FIXME: move that back to the parsing area

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_route, surf, "Routing part of surf");

namespace simgrid {
namespace kernel {
namespace routing {

  /* Callbacks */
  simgrid::xbt::signal<void(NetCard*)> netcardCreatedCallbacks;
  simgrid::xbt::signal<void(s4u::As*)> asCreatedCallbacks;

}}} // namespace simgrid::kernel::routing

/**
 * @ingroup SURF_build_api
 * @brief A library containing all known hosts
 */

int COORD_HOST_LEVEL = -1;         //Coordinates level

int MSG_FILE_LEVEL = -1;             //Msg file level

int SIMIX_STORAGE_LEVEL = -1;        //Simix storage level
int MSG_STORAGE_LEVEL = -1;          //Msg storage level

xbt_lib_t as_router_lib;
int ROUTING_ASR_LEVEL = -1;          //Routing level
int COORD_ASR_LEVEL = -1;            //Coordinates level
int NS3_ASR_LEVEL = -1;              //host node for ns3
int ROUTING_PROP_ASR_LEVEL = -1;     //Where the properties are stored

/** @brief Retrieve a netcard from its name
 *
 * Netcards are the thing that connect host or routers to the network
 */
simgrid::kernel::routing::NetCard *sg_netcard_by_name_or_null(const char *name)
{
  sg_host_t h = sg_host_by_name(name);
  simgrid::kernel::routing::NetCard *netcard = h==nullptr ? nullptr: h->pimpl_netcard;
  if (!netcard)
    netcard = (simgrid::kernel::routing::NetCard*) xbt_lib_get_or_null(as_router_lib, name, ROUTING_ASR_LEVEL);
  return netcard;
}

/* Global vars */
simgrid::kernel::routing::RoutingPlatf *routing_platf = nullptr;


void sg_platf_new_trace(sg_platf_trace_cbarg_t trace)
{
  tmgr_trace_t tmgr_trace;
  if (trace->file && strcmp(trace->file, "") != 0) {
    tmgr_trace = tmgr_trace_new_from_file(trace->file);
  } else {
    xbt_assert(strcmp(trace->pc_data, ""),
        "Trace '%s' must have either a content, or point to a file on disk.",trace->id);
    tmgr_trace = tmgr_trace_new_from_string(trace->id, trace->pc_data, trace->periodicity);
  }
  xbt_dict_set(traces_set_list, trace->id, (void *) tmgr_trace, nullptr);
}

namespace simgrid {
namespace kernel {
namespace routing {

/**
 * \brief Find a route between hosts
 *
 * \param src the network_element_t for src host
 * \param dst the network_element_t for dst host
 * \param route where to store the list of links.
 *              If *route=nullptr, create a short lived dynar. Else, fill the provided dynar
 * \param latency where to store the latency experienced on the path (or nullptr if not interested)
 *                It is the caller responsibility to initialize latency to 0 (we add to provided route)
 * \pre route!=nullptr
 *
 * walk through the routing components tree and find a route between hosts
 * by calling each "get_route" function in each routing component.
 */
void RoutingPlatf::getRouteAndLatency(NetCard *src, NetCard *dst, std::vector<Link*> * route, double *latency)
{
  XBT_DEBUG("getRouteAndLatency from %s to %s", src->name(), dst->name());

  AsImpl::getRouteRecursive(src, dst, route, latency);
}

static xbt_dynar_t _recursiveGetOneLinkRoutes(AsImpl *as)
{
  xbt_dynar_t ret = xbt_dynar_new(sizeof(Onelink*), xbt_free_f);

  //adding my one link routes
  xbt_dynar_t onelink_mine = as->getOneLinkRoutes();
  if (onelink_mine)
    xbt_dynar_merge(&ret,&onelink_mine);

  //recursing
  char *key;
  xbt_dict_cursor_t cursor = nullptr;
  AsImpl *rc_child;
  xbt_dict_foreach(as->children(), cursor, key, rc_child) {
    xbt_dynar_t onelink_child = _recursiveGetOneLinkRoutes(rc_child);
    if (onelink_child)
      xbt_dynar_merge(&ret,&onelink_child);
  }
  return ret;
}

xbt_dynar_t RoutingPlatf::getOneLinkRoutes(){
  return _recursiveGetOneLinkRoutes(root_);
}

}}}

/** @brief create the root AS */
void routing_model_create(Link *loopback)
{
  routing_platf = new simgrid::kernel::routing::RoutingPlatf(loopback);
}

/* ************************************************************************** */
/* ************************* GENERIC PARSE FUNCTIONS ************************ */

static void check_disk_attachment()
{
  xbt_lib_cursor_t cursor;
  char *key;
  void **data;
  simgrid::kernel::routing::NetCard *host_elm;
  xbt_lib_foreach(storage_lib, cursor, key, data) {
    if(xbt_lib_get_level(xbt_lib_get_elm_or_null(storage_lib, key), SURF_STORAGE_LEVEL) != nullptr) {
    simgrid::surf::Storage *storage = static_cast<simgrid::surf::Storage*>(xbt_lib_get_level(xbt_lib_get_elm_or_null(storage_lib, key), SURF_STORAGE_LEVEL));
    host_elm = sg_netcard_by_name_or_null(storage->attach_);
    if(!host_elm)
      surf_parse_error("Unable to attach storage %s: host %s doesn't exist.", storage->getName(), storage->attach_);
    }
  }
}

void routing_register_callbacks()
{
  simgrid::surf::on_postparse.connect(check_disk_attachment);

  instr_routing_define_callbacks();
}

/** \brief Frees all memory allocated by the routing module */
void routing_exit() {
  delete routing_platf;
}

simgrid::kernel::routing::RoutingPlatf::RoutingPlatf(simgrid::surf::Link *loopback)
: loopback_(loopback)
{
}
simgrid::kernel::routing::RoutingPlatf::~RoutingPlatf()
{
  delete root_;
}
