/* Copyright (c) 2009-2016. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "xbt/dict.h"
#include "xbt/log.h"
#include "xbt/sysdep.h"
#include "xbt/dynar.h"
#include "xbt/graph.h"

#include "src/kernel/routing/AsRoutedGraph.hpp"
#include "src/surf/network_interface.hpp"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_routing_generic, surf_route, "Generic implementation of the surf routing");

void routing_route_free(sg_platf_route_cbarg_t route)
{
  if (route) {
    delete route->link_list;
    xbt_free(route);
  }
}

namespace simgrid {
namespace kernel {
namespace routing {

AsRoutedGraph::AsRoutedGraph(As* father, const char* name) : AsImpl(father, name)
{
}

}}} // namespace simgrid::kernel::routing

/* ************************************************************************** */
/* *********************** GENERIC BUSINESS METHODS ************************* */

static const char *instr_node_name(xbt_node_t node)
{
  void *data = xbt_graph_node_get_data(node);
  char *str = (char *) data;
  return str;
}

xbt_node_t new_xbt_graph_node(xbt_graph_t graph, const char *name, xbt_dict_t nodes)
{
  xbt_node_t ret = (xbt_node_t) xbt_dict_get_or_null(nodes, name);
  if (ret)
    return ret;

  ret = xbt_graph_new_node(graph, xbt_strdup(name));
  xbt_dict_set(nodes, name, ret, nullptr);
  return ret;
}

xbt_edge_t new_xbt_graph_edge(xbt_graph_t graph, xbt_node_t s, xbt_node_t d, xbt_dict_t edges)
{
  const char *sn = instr_node_name(s);
  const char *dn = instr_node_name(d);
  int len = strlen(sn) + strlen(dn) + 1;
  char *name = (char *) xbt_malloc(len * sizeof(char));

  snprintf(name, len, "%s%s", sn, dn);
  xbt_edge_t ret = (xbt_edge_t) xbt_dict_get_or_null(edges, name);
  if (ret == nullptr) {
    snprintf(name, len, "%s%s", dn, sn);
    ret = (xbt_edge_t) xbt_dict_get_or_null(edges, name);
  }

  if (ret == nullptr) {
    ret = xbt_graph_new_edge(graph, s, d, nullptr);
    xbt_dict_set(edges, name, ret, nullptr);
  }
  free(name);
  return ret;
}

namespace simgrid {
namespace kernel {
namespace routing {

void AsRoutedGraph::getOneLinkRoutes(std::vector<Onelink*>* accumulator)
{
  sg_platf_route_cbarg_t route = xbt_new0(s_sg_platf_route_cbarg_t, 1);
  route->link_list             = new std::vector<Link*>();

  int table_size = static_cast<int>(vertices_.size());
  for (int src = 0; src < table_size; src++) {
    for (int dst = 0; dst < table_size; dst++) {
      route->link_list->clear();
      NetCard* src_elm = vertices_.at(src);
      NetCard* dst_elm = vertices_.at(dst);
      this->getLocalRoute(src_elm, dst_elm, route, nullptr);

      if (route->link_list->size() == 1) {
        Link* link = route->link_list->at(0);
        Onelink* onelink;
        if (hierarchy_ == RoutingMode::base)
          onelink = new Onelink(link, src_elm, dst_elm);
        else if (hierarchy_ == RoutingMode::recursive)
          onelink = new Onelink(link, route->gw_src, route->gw_dst);
        else
          onelink = new Onelink(link, nullptr, nullptr);
        accumulator->push_back(onelink);
      }
    }
  }
  AsImpl::getOneLinkRoutes(accumulator); // Recursivly call this function on all my childs too
}

void AsRoutedGraph::getGraph(xbt_graph_t graph, xbt_dict_t nodes, xbt_dict_t edges)
{
  for (auto my_src: vertices_){
    for (auto my_dst: vertices_){
      if (my_src == my_dst)
        continue;

      sg_platf_route_cbarg_t route = xbt_new0(s_sg_platf_route_cbarg_t, 1);
      route->link_list = new std::vector<Link*>();

      getLocalRoute(my_src, my_dst, route, nullptr);

      XBT_DEBUG("get_route_and_latency %s -> %s", my_src->name().c_str(), my_dst->name().c_str());

      xbt_node_t current, previous;
      const char *previous_name, *current_name;

      if (route->gw_src) {
        previous      = new_xbt_graph_node(graph, route->gw_src->name().c_str(), nodes);
        previous_name = route->gw_src->name().c_str();
      } else {
        previous      = new_xbt_graph_node(graph, my_src->name().c_str(), nodes);
        previous_name = my_src->name().c_str();
      }

      for (auto link: *route->link_list) {
        const char *link_name = link->getName();
        current = new_xbt_graph_node(graph, link_name, nodes);
        current_name = link_name;
        new_xbt_graph_edge(graph, previous, current, edges);
        XBT_DEBUG ("  %s -> %s", previous_name, current_name);
        previous = current;
        previous_name = current_name;
      }

      if (route->gw_dst) {
        current      = new_xbt_graph_node(graph, route->gw_dst->name().c_str(), nodes);
        current_name = route->gw_dst->name().c_str();
      } else {
        current      = new_xbt_graph_node(graph, my_dst->name().c_str(), nodes);
        current_name = my_dst->name().c_str();
      }
      new_xbt_graph_edge(graph, previous, current, edges);
      XBT_DEBUG ("  %s -> %s", previous_name, current_name);

      delete route->link_list;
      xbt_free (route);
    }
  }
}

/* ************************************************************************** */
/* ************************* GENERIC AUX FUNCTIONS ************************** */
/* change a route containing link names into a route containing link entities */
sg_platf_route_cbarg_t AsRoutedGraph::newExtendedRoute(RoutingMode hierarchy, sg_platf_route_cbarg_t routearg, int change_order)
{
  sg_platf_route_cbarg_t result;

  result = xbt_new0(s_sg_platf_route_cbarg_t, 1);
  result->link_list = new std::vector<Link*>();

  xbt_assert(hierarchy == RoutingMode::base || hierarchy == RoutingMode::recursive,
      "The hierarchy of this AS is neither BASIC nor RECURSIVE, I'm lost here.");

  if (hierarchy == RoutingMode::recursive) {
    xbt_assert(routearg->gw_src && routearg->gw_dst, "nullptr is obviously a deficient gateway");

    result->gw_src = routearg->gw_src;
    result->gw_dst = routearg->gw_dst;
  }

  for (auto link : *routearg->link_list) {
    if (change_order)
      result->link_list->push_back(link);
    else
      result->link_list->insert(result->link_list->begin(), link);
  }

  return result;
}

void AsRoutedGraph::getRouteCheckParams(NetCard *src, NetCard *dst)
{
  xbt_assert(src, "Cannot find a route from nullptr to %s", dst->name().c_str());
  xbt_assert(dst, "Cannot find a route from %s to nullptr", src->name().c_str());

  As *src_as = src->containingAS();
  As *dst_as = dst->containingAS();

  xbt_assert(src_as == dst_as,
             "Internal error: %s@%s and %s@%s are not in the same AS as expected. Please report that bug.",
             src->name().c_str(), src_as->name(), dst->name().c_str(), dst_as->name());

  xbt_assert(this == dst_as, "Internal error: route destination %s@%s is not in AS %s as expected (route source: "
                             "%s@%s). Please report that bug.",
             src->name().c_str(), dst->name().c_str(), src_as->name(), dst_as->name(), name());
}
void AsRoutedGraph::addRouteCheckParams(sg_platf_route_cbarg_t route) {
  NetCard *src = route->src;
  NetCard *dst = route->dst;
  const char* srcName = src->name().c_str();
  const char* dstName = dst->name().c_str();

  if(!route->gw_dst && !route->gw_src) {
    XBT_DEBUG("Load Route from \"%s\" to \"%s\"", srcName, dstName);
    xbt_assert(src, "Cannot add a route from %s to %s: %s does not exist.", srcName, dstName, srcName);
    xbt_assert(dst, "Cannot add a route from %s to %s: %s does not exist.", srcName, dstName, dstName);
    xbt_assert(! route->link_list->empty(), "Empty route (between %s and %s) forbidden.", srcName, dstName);
    xbt_assert(! src->isAS(), "When defining a route, src cannot be an AS such as '%s'. Did you meant to have an ASroute?", srcName);
    xbt_assert(! dst->isAS(), "When defining a route, dst cannot be an AS such as '%s'. Did you meant to have an ASroute?", dstName);
  } else {
    XBT_DEBUG("Load ASroute from %s@%s to %s@%s", srcName, route->gw_src->name().c_str(), dstName,
              route->gw_dst->name().c_str());
    xbt_assert(src->isAS(), "When defining an ASroute, src must be an AS but '%s' is not", srcName);
    xbt_assert(dst->isAS(), "When defining an ASroute, dst must be an AS but '%s' is not", dstName);

    xbt_assert(route->gw_src->isHost() || route->gw_src->isRouter(),
        "When defining an ASroute, gw_src must be an host or a router but '%s' is not.", srcName);
    xbt_assert(route->gw_dst->isHost() || route->gw_dst->isRouter(),
        "When defining an ASroute, gw_dst must be an host or a router but '%s' is not.", dstName);

    xbt_assert(route->gw_src != route->gw_dst, "Cannot define an ASroute from '%s' to itself",
               route->gw_src->name().c_str());

    xbt_assert(src, "Cannot add a route from %s@%s to %s@%s: %s does not exist.", srcName,
               route->gw_src->name().c_str(), dstName, route->gw_dst->name().c_str(), srcName);
    xbt_assert(dst, "Cannot add a route from %s@%s to %s@%s: %s does not exist.", srcName,
               route->gw_src->name().c_str(), dstName, route->gw_dst->name().c_str(), dstName);
    xbt_assert(!route->link_list->empty(), "Empty route (between %s@%s and %s@%s) forbidden.", srcName,
               route->gw_src->name().c_str(), dstName, route->gw_dst->name().c_str());
  }
}

}}}
