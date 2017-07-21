/* Copyright (c) 2006-2017. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "xbt/log.h"

#include "simgrid/s4u/Host.hpp"
#include "simgrid/s4u/NetZone.hpp"
#include "simgrid/simix.hpp"
#include "src/kernel/routing/NetPoint.hpp"
#include "src/surf/network_interface.hpp" // Link FIXME: move to proper header

XBT_LOG_NEW_DEFAULT_CATEGORY(s4u_netzone, "S4U Networking Zones");

namespace simgrid {
namespace s4u {

simgrid::xbt::signal<void(bool symmetrical, kernel::routing::NetPoint* src, kernel::routing::NetPoint* dst,
                          kernel::routing::NetPoint* gw_src, kernel::routing::NetPoint* gw_dst,
                          std::vector<surf::LinkImpl*>* link_list)>
    NetZone::onRouteCreation;
simgrid::xbt::signal<void(NetZone&)> NetZone::onCreation;
simgrid::xbt::signal<void(NetZone&)> NetZone::onSeal;

NetZone::NetZone(NetZone* father, const char* name) : father_(father), name_(xbt_strdup(name))
{
  children_ = new std::vector<NetZone*>();
}

void NetZone::seal()
{
  sealed_ = true;
}

NetZone::~NetZone()
{
  for (auto nz : *children_)
    delete nz;
  delete children_;
  xbt_free(name_);
}

std::unordered_map<std::string, std::string>* NetZone::getProperties()
{
  return simgrid::simix::kernelImmediate([this] {
      return &properties_;
  });
}

/** Retrieve the property value (or nullptr if not set) */
const char* NetZone::getProperty(const char* key)
{
  return properties_.at(key).c_str();
}
void NetZone::setProperty(const char* key, const char* value)
{
  simgrid::simix::kernelImmediate([this,key,value] {
    properties_[key] = value;
  });
}

std::vector<NetZone*>* NetZone::getChildren()
{
  return children_;
}
char* NetZone::getCname()
{
  return name_;
}
NetZone* NetZone::getFather()
{
  return father_;
}

void NetZone::getHosts(std::vector<s4u::Host*>* whereto)
{
  for (auto card : vertices_) {
    s4u::Host* host = simgrid::s4u::Host::by_name_or_null(card->name());
    if (host != nullptr)
      whereto->push_back(host);
  }
}

int NetZone::addComponent(kernel::routing::NetPoint* elm)
{
  vertices_.push_back(elm);
  return vertices_.size() - 1; // The rank of the newly created object
}

void NetZone::addRoute(sg_platf_route_cbarg_t /*route*/)
{
  xbt_die("NetZone '%s' does not accept new routes (wrong class).", name_);
}
}
}; // namespace simgrid::s4u
