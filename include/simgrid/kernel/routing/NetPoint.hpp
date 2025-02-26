/* Copyright (c) 2013-2025. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef KERNEL_ROUTING_NETPOINT_HPP_
#define KERNEL_ROUTING_NETPOINT_HPP_

#include <xbt/Extendable.hpp>
#include <xbt/base.h>
#include <xbt/signal.hpp>

#include <simgrid/kernel/routing/NetZoneImpl.hpp>

namespace simgrid {

extern template class XBT_PUBLIC xbt::Extendable<kernel::routing::NetPoint>;

namespace kernel::routing {

/** @ingroup ROUTING_API
 *  @brief Network cards are the vertices in the graph representing the network, used to compute paths between nodes.
 *
 * @details This represents a position in the network. One can send information between two netpoints
 */
class NetPoint : public xbt::Extendable<NetPoint> {
public:
  enum class Type { Host, Router, NetZone };

  NetPoint(const std::string& name, NetPoint::Type component_type);

  // Our rank in the vertices_ array of the netzone that contains us.
  unsigned long id() const { return id_; }
  const std::string& get_name() const { return name_; }
  const char* get_cname() const { return name_.c_str(); }
  /** @brief the NetZone in which this NetPoint is included */
  NetZoneImpl* get_englobing_zone() const { return englobing_zone_; }
  /** @brief Returns the NetZones that contain the NetPoint, from root to leaf */
  std::vector<NetZoneImpl*> get_englobing_zones() const;
  /** @brief Set the NetZone in which this NetPoint is included */
  NetPoint* set_englobing_zone(NetZoneImpl* netzone_p);
  NetPoint* set_coordinates(const std::string& coords);

  bool is_netzone() const { return component_type_ == Type::NetZone; }
  bool is_host() const { return component_type_ == Type::Host; }
  bool is_router() const { return component_type_ == Type::Router; }

  static xbt::signal<void(NetPoint&)> on_creation;

  bool operator<(const NetPoint& rhs) const { return name_ < rhs.name_; }

private:
  unsigned long id_ = -1;
  std::string name_;
  NetPoint::Type component_type_;
  NetZoneImpl* englobing_zone_ = nullptr;
};
} // namespace kernel::routing
} // namespace simgrid

XBT_PUBLIC simgrid::kernel::routing::NetPoint* sg_netpoint_by_name_or_null(const char* name);

#endif /* KERNEL_ROUTING_NETPOINT_HPP_ */
