/* Copyright (c) 2006-2014. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/log.h>

#include <simgrid/s4u/host.hpp>
#include <simgrid/s4u/As.hpp>

#include "src/surf/surf_routing.hpp"
#include "src/surf/network_interface.hpp" // Link FIXME: move to proper header

XBT_LOG_NEW_DEFAULT_CATEGORY(s4u_as,"S4U autonomous systems");

namespace simgrid {
  namespace s4u {

    As::As(const char *name)
    : name_(xbt_strdup(name))
    {
    }
    void As::seal()
    {
      sealed_ = true;
    }
    As::~As()
    {
      xbt_dict_cursor_t cursor = nullptr;
      char *key;
      AS_t elem;
      xbt_dict_foreach(children_, cursor, key, elem) {
        delete (As*)elem;
      }


      xbt_dict_free(&children_);
      for (auto &kv : bypassRoutes_)
        delete kv.second;
      xbt_free(name_);
    }

    xbt_dict_t As::children()
    {
      return children_;
    }
    char *As::name()
    {
      return name_;
    }
    As *As::father() {
      return father_;
    }

    xbt_dynar_t As::hosts()
    {
      xbt_dynar_t res =  xbt_dynar_new(sizeof(sg_host_t), nullptr);

      for (auto card : vertices_) {
        s4u::Host *host = simgrid::s4u::Host::by_name_or_null(card->name());
        if (host!=nullptr)
          xbt_dynar_push(res, &host);
      }
      return res;
    }

    int As::addComponent(kernel::routing::NetCard *elm) {
      vertices_.push_back(elm);
      return vertices_.size()-1; // The rank of the newly created object
    }

    void As::addRoute(sg_platf_route_cbarg_t /*route*/){
      xbt_die("AS %s does not accept new routes (wrong class).",name_);
    }

    void As::addBypassRoute(sg_platf_route_cbarg_t e_route){
      /* Argument validity checks */
      if (e_route->gw_dst) {
        XBT_DEBUG("Load bypassASroute from %s@%s to %s@%s",
            e_route->src->name(), e_route->gw_src->name(),
            e_route->dst->name(), e_route->gw_dst->name());
        xbt_assert(!e_route->link_list->empty(), "Bypass route between %s@%s and %s@%s cannot be empty.",
            e_route->src->name(), e_route->gw_src->name(),
            e_route->dst->name(), e_route->gw_dst->name());
        xbt_assert(bypassRoutes_.find({e_route->src->name(),e_route->dst->name()}) == bypassRoutes_.end(),
            "The bypass route between %s@%s and %s@%s already exists.",
            e_route->src->name(), e_route->gw_src->name(), e_route->dst->name(), e_route->gw_dst->name());
      } else {
        XBT_DEBUG("Load bypassRoute from %s to %s", e_route->src->name(), e_route->dst->name());
        xbt_assert(!e_route->link_list->empty(),                         "Bypass route between %s and %s cannot be empty.",
            e_route->src->name(), e_route->dst->name());
        xbt_assert(bypassRoutes_.find({e_route->src->name(),e_route->dst->name()}) == bypassRoutes_.end(),
            "The bypass route between %s and %s already exists.",
            e_route->src->name(), e_route->dst->name());
      }

      /* Build a copy that will be stored in the dict */
      std::vector<surf::Link*> *newRoute = new std::vector<surf::Link*>();
      for (auto link: *e_route->link_list)
        newRoute->push_back(link);

      /* Store it */
      bypassRoutes_.insert({{e_route->src->name(),e_route->dst->name()}, newRoute});
    }

}  }; // namespace simgrid::s4u
