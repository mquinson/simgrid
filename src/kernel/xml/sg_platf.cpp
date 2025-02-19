/* Copyright (c) 2006-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

/* This file implements the public API to platform parsing                  */

#include <simgrid/Exception.hpp>
#include <simgrid/kernel/routing/DijkstraZone.hpp>
#include <simgrid/kernel/routing/DragonflyZone.hpp>
#include <simgrid/kernel/routing/EmptyZone.hpp>
#include <simgrid/kernel/routing/FatTreeZone.hpp>
#include <simgrid/kernel/routing/FloydZone.hpp>
#include <simgrid/kernel/routing/FullZone.hpp>
#include <simgrid/kernel/routing/NetPoint.hpp>
#include <simgrid/kernel/routing/NetZoneImpl.hpp>
#include <simgrid/kernel/routing/TorusZone.hpp>
#include <simgrid/kernel/routing/VivaldiZone.hpp>
#include <simgrid/kernel/routing/WifiZone.hpp>
#include <simgrid/s4u/Engine.hpp>
#include <simgrid/s4u/NetZone.hpp>

#include "src/kernel/EngineImpl.hpp"
#include "src/kernel/resource/DiskImpl.hpp"
#include "src/kernel/resource/HostImpl.hpp"
#include "src/kernel/resource/StandardLinkImpl.hpp"
#include "src/kernel/resource/profile/Profile.hpp"
#include "src/kernel/xml/platf.hpp"
#include "src/kernel/xml/platf_private.hpp"
#include "src/simgrid/sg_config.hpp"

#include <algorithm>
#include <string>

XBT_LOG_EXTERNAL_DEFAULT_CATEGORY(platf_parse);

/* This function acts as a main in the parsing area. */
void parse_platform_file(const std::string& file)
{
  /* init the flex parser */
  simgrid_parse_open(file);

  /* Do the actual parsing */
  simgrid_parse(true);

  simgrid_parse_close();
}

namespace simgrid::kernel::routing {
xbt::signal<void(ClusterCreationArgs const&)> on_cluster_creation;
} // namespace simgrid::kernel::routing

static simgrid::kernel::routing::ClusterZoneCreationArgs
    zone_cluster; /* temporary store data for irregular clusters, created with <zone routing="Cluster"> */

/** The current NetZone in the parsing */
static simgrid::kernel::routing::NetZoneImpl* current_routing = nullptr;
static simgrid::s4u::Host* current_host                       = nullptr;

/** Module management function: frees all internal data structures */
void sg_platf_parser_finalize()
{
  simgrid::kernel::routing::on_cluster_creation.disconnect_slots();

  simgrid_parse_lex_destroy();
}

/** @brief Add a host to the current NetZone */
void sg_platf_new_host_begin(const simgrid::kernel::routing::HostCreationArgs* args)
{
  current_host = current_routing->create_host(args->id, args->speed_per_pstate)
                     ->set_coordinates(args->coord)
                     ->set_core_count(args->core_amount)
                     ->set_state_profile(args->state_trace)
                     ->set_speed_profile(args->speed_trace);
}

void sg_platf_new_host_set_properties(const std::unordered_map<std::string, std::string>& props)
{
  xbt_assert(current_host, "Cannot set properties of the current host: none under construction");
  current_host->set_properties(props);
}

void sg_platf_new_host_seal(int pstate)
{
  xbt_assert(current_host, "Cannot seal the current Host: none under construction");
  current_host->seal();

  /* When energy plugin is activated, changing the pstate requires to already have the HostEnergy extension whose
   * allocation is triggered by the on_creation signal. Then set_pstate must be called after the signal emission */

  if (pstate != 0)
    current_host->set_pstate(pstate);

  current_host = nullptr;
}

void sg_platf_new_peer(const simgrid::kernel::routing::PeerCreationArgs* args)
{
  auto* zone = dynamic_cast<simgrid::kernel::routing::VivaldiZone*>(current_routing);
  xbt_assert(zone, "<peer> tag can only be used in Vivaldi netzones.");

  const auto* peer = zone->create_host(args->id, {args->speed})
                         ->set_state_profile(args->state_trace)
                         ->set_speed_profile(args->speed_trace)
                         ->set_coordinates(args->coord)
                         ->seal();

  zone->set_peer_link(peer->get_netpoint(), args->bw_in, args->bw_out);
}

/** @brief Add a "router" to the network element list */
simgrid::kernel::routing::NetPoint* sg_platf_new_router(const std::string& name, const std::string& coords)
{
  auto* netpoint = current_routing->create_router(name)->set_coordinates(coords);
  XBT_DEBUG("Router '%s' has the id %lu", netpoint->get_cname(), netpoint->id());

  return netpoint;
}

void sg_platf_new_link(const simgrid::kernel::routing::LinkCreationArgs* args)
{
  simgrid::s4u::Link* link;
  if (args->policy == simgrid::s4u::Link::SharingPolicy::SPLITDUPLEX) {
    link = current_routing->create_split_duplex_link(args->id, args->bandwidths, args->bandwidths);
  } else {
    link = current_routing->create_link(args->id, args->bandwidths);
    link->get_impl()->set_sharing_policy(args->policy, {});
  }

  link->set_properties(args->properties)
      ->set_state_profile(args->state_trace)
      ->set_latency_profile(args->latency_trace)
      ->set_bandwidth_profile(args->bandwidth_trace)
      ->set_latency(args->latency);
}

void sg_platf_new_disk(const simgrid::kernel::routing::DiskCreationArgs* disk)
{
  const simgrid::s4u::Disk* new_disk = current_routing->create_disk(disk->id, disk->read_bw, disk->write_bw)
                                           ->set_host(current_host)
                                           ->set_properties(disk->properties)
                                           ->seal();

  current_host->add_disk(new_disk);
}

/*************************************************************************************************/
/** @brief Auxiliary function to create hosts */
static simgrid::s4u::Host*
sg_platf_cluster_create_host(const simgrid::kernel::routing::ClusterCreationArgs* cluster, simgrid::s4u::NetZone* zone,
                             const std::vector<unsigned long>& /*coord*/, unsigned long id)
{
  xbt_assert(static_cast<unsigned long>(id) < cluster->radicals.size(),
             "Zone(%s): error when creating host number %lu in the zone. Insufficient number of radicals available "
             "(total = %zu). Check the 'radical' parameter in XML",
             cluster->id.c_str(), id, cluster->radicals.size());

  std::string host_id = cluster->prefix + std::to_string(cluster->radicals[id]) + cluster->suffix;
  XBT_DEBUG("Cluster: creating host=%s speed=%f", host_id.c_str(), cluster->speeds.front());
  simgrid::s4u::Host* host = zone->create_host(host_id, cluster->speeds)
                                 ->set_core_count(cluster->core_amount)
                                 ->set_properties(cluster->properties);
  return host;
}

/** @brief Auxiliary function to create loopback links */
static simgrid::s4u::Link*
sg_platf_cluster_create_loopback(const simgrid::kernel::routing::ClusterCreationArgs* cluster,
                                 simgrid::s4u::NetZone* zone, const std::vector<unsigned long>& /*coord*/,
                                 unsigned long id)
{
  xbt_assert(static_cast<unsigned long>(id) < cluster->radicals.size(),
             "Zone(%s): error when creating loopback for host number %lu in the zone. Insufficient number of "
             "radicals available "
             "(total = %zu). Check the 'radical' parameter in XML",
             cluster->id.c_str(), id, cluster->radicals.size());

  std::string link_id = cluster->id + "_link_" + std::to_string(cluster->radicals[id]) + "_loopback";
  XBT_DEBUG("Cluster: creating loopback link=%s bw=%f", link_id.c_str(), cluster->loopback_bw);

  simgrid::s4u::Link* loopback = zone->create_link(link_id, cluster->loopback_bw)
                                     ->set_sharing_policy(simgrid::s4u::Link::SharingPolicy::FATPIPE)
                                     ->set_latency(cluster->loopback_lat)
                                     ->seal();
  return loopback;
}

/** @brief Auxiliary function to create limiter links */
static simgrid::s4u::Link* sg_platf_cluster_create_limiter(const simgrid::kernel::routing::ClusterCreationArgs* cluster,
                                                           simgrid::s4u::NetZone* zone,
                                                           const std::vector<unsigned long>& /*coord*/,
                                                           unsigned long id)
{
  std::string link_id = cluster->id + "_link_" + std::to_string(id) + "_limiter";
  XBT_DEBUG("Cluster: creating limiter link=%s bw=%f", link_id.c_str(), cluster->limiter_link);

  simgrid::s4u::Link* limiter = zone->create_link(link_id, cluster->limiter_link)->seal();
  return limiter;
}

/** @brief Create Torus, Fat-Tree and Dragonfly clusters */
static void sg_platf_new_cluster_hierarchical(const simgrid::kernel::routing::ClusterCreationArgs* cluster)
{
  using namespace std::placeholders;
  using simgrid::kernel::routing::DragonflyZone;
  using simgrid::kernel::routing::FatTreeZone;
  using simgrid::kernel::routing::TorusZone;

  std::function<simgrid::s4u::ClusterCallbacks::ClusterHostCb> set_host =
    std::bind(sg_platf_cluster_create_host, cluster, _1, _2, _3);
  std::function<simgrid::s4u::ClusterCallbacks::ClusterLinkCb> set_loopback{};
  std::function<simgrid::s4u::ClusterCallbacks::ClusterLinkCb> set_limiter{};

  if (cluster->loopback_bw > 0 || cluster->loopback_lat > 0) {
    set_loopback = std::bind(sg_platf_cluster_create_loopback, cluster, _1, _2, _3);
  }

  if (cluster->limiter_link > 0) {
    set_limiter = std::bind(sg_platf_cluster_create_limiter, cluster, _1, _2, _3);
  }

  simgrid::s4u::NetZone const* parent = current_routing ? current_routing->get_iface() : nullptr;
  switch (cluster->topology) {
    case simgrid::kernel::routing::ClusterTopology::TORUS:
      simgrid::s4u::create_torus_zone(cluster->id, parent, TorusZone::parse_topo_parameters(cluster->topo_parameters),
                                      {set_host, set_loopback, set_limiter}, cluster->bw, cluster->lat,
                                      cluster->sharing_policy);
      break;
    case simgrid::kernel::routing::ClusterTopology::DRAGONFLY:
      simgrid::s4u::create_dragonfly_zone(
          cluster->id, parent, DragonflyZone::parse_topo_parameters(cluster->topo_parameters),
          {set_host, set_loopback, set_limiter}, cluster->bw, cluster->lat, cluster->sharing_policy);
      break;
    case simgrid::kernel::routing::ClusterTopology::FAT_TREE:
      simgrid::s4u::create_fatTree_zone(
          cluster->id, parent, FatTreeZone::parse_topo_parameters(cluster->topo_parameters),
          {set_host, set_loopback, set_limiter}, cluster->bw, cluster->lat, cluster->sharing_policy);
      break;
    default:
      THROW_IMPOSSIBLE;
  }
}

/*************************************************************************************************/
/** @brief Create regular Cluster */
static void sg_platf_new_cluster_flat(simgrid::kernel::routing::ClusterCreationArgs* cluster)
{
  simgrid::s4u::NetZone* parent;
  if (current_routing != nullptr)
    parent = current_routing->get_iface();
  else
    parent = simgrid::s4u::Engine::get_instance()->get_netzone_root();
  simgrid::s4u::NetZone* zone = parent->add_netzone_star(cluster->id);

  /* Make the backbone */
  const simgrid::s4u::Link* backbone = nullptr;
  if ((cluster->bb_bw > 0) || (cluster->bb_lat > 0)) {
    std::string bb_name = cluster->id + "_backbone";
    XBT_DEBUG("<link\tid=\"%s\" bw=\"%f\" lat=\"%f\"/> <!--backbone -->", bb_name.c_str(), cluster->bb_bw,
              cluster->bb_lat);

    backbone = zone->create_link(bb_name, cluster->bb_bw)
                   ->set_sharing_policy(cluster->bb_sharing_policy)
                   ->set_latency(cluster->bb_lat)
                   ->seal();
  }

  for (int const& i : cluster->radicals) {
    std::string host_id = cluster->prefix + std::to_string(i) + cluster->suffix;

    XBT_DEBUG("<host\tid=\"%s\"\tspeed=\"%f\">", host_id.c_str(), cluster->speeds.front());
    const auto* host = zone->create_host(host_id, cluster->speeds)
                           ->set_core_count(cluster->core_amount)
                           // Cluster properties are attached to each host instead of to the cluster zone
                           ->set_properties(cluster->properties)
                           ->seal();

    XBT_DEBUG("</host>");

    std::string link_id = cluster->id + "_link_" + std::to_string(i);
    XBT_DEBUG("<link\tid=\"%s\"\tbw=\"%f\"\tlat=\"%f\"/>", link_id.c_str(), cluster->bw, cluster->lat);

    // add a loopback link
    if (cluster->loopback_bw > 0 || cluster->loopback_lat > 0) {
      std::string loopback_name = link_id + "_loopback";
      XBT_DEBUG("<loopback\tid=\"%s\"\tbw=\"%f\"/>", loopback_name.c_str(), cluster->loopback_bw);

      const auto* loopback = zone->create_link(loopback_name, cluster->loopback_bw)
                                 ->set_sharing_policy(simgrid::s4u::Link::SharingPolicy::FATPIPE)
                                 ->set_latency(cluster->loopback_lat)
                                 ->seal();

      zone->add_route(host, host, {simgrid::s4u::LinkInRoute(loopback)});
    }

    // add a limiter link (shared link to account for maximal bandwidth of the node)
    const simgrid::s4u::Link* limiter = nullptr;
    if (cluster->limiter_link > 0) {
      std::string limiter_name = link_id + "_limiter";
      XBT_DEBUG("<limiter\tid=\"%s\"\tbw=\"%f\"/>", limiter_name.c_str(), cluster->limiter_link);

      limiter = zone->create_link(limiter_name, cluster->limiter_link)->seal();
    }

    // create link
    const simgrid::s4u::Link* link;
    if (cluster->sharing_policy == simgrid::s4u::Link::SharingPolicy::SPLITDUPLEX) {
      link = zone->create_split_duplex_link(link_id, cluster->bw, cluster->bw)->set_latency(cluster->lat)->seal();
    } else {
      link = zone->create_link(link_id, cluster->bw)->set_latency(cluster->lat)->seal();
    }

    /* adding routes */
    std::vector<simgrid::s4u::LinkInRoute> links;
    if (limiter)
      links.emplace_back(limiter);
    links.emplace_back(link, simgrid::s4u::LinkInRoute::Direction::UP);
    if (backbone)
      links.emplace_back(backbone);

    zone->add_route(host, nullptr, links, true);
  }

  // Add a router.
  XBT_DEBUG(" ");
  XBT_DEBUG("<router id=\"%s\"/>", cluster->router_id.c_str());
  if (cluster->router_id.empty())
    cluster->router_id = cluster->prefix + cluster->id + "_router" + cluster->suffix;
  zone->create_router(cluster->router_id);

  simgrid::kernel::routing::on_cluster_creation(*cluster);
}

void sg_platf_new_tag_cluster(simgrid::kernel::routing::ClusterCreationArgs* cluster)
{
  switch (cluster->topology) {
    case simgrid::kernel::routing::ClusterTopology::TORUS:
    case simgrid::kernel::routing::ClusterTopology::DRAGONFLY:
    case simgrid::kernel::routing::ClusterTopology::FAT_TREE:
      sg_platf_new_cluster_hierarchical(cluster);
      break;
    default:
      sg_platf_new_cluster_flat(cluster);
      break;
  }
}
/*************************************************************************************************/
/** @brief Set the links for internal node inside a Cluster(Star) */
static void sg_platf_cluster_set_hostlink(simgrid::kernel::routing::StarZone* zone,
                                          simgrid::kernel::routing::NetPoint* netpoint,
                                          const simgrid::s4u::Link* link_up, const simgrid::s4u::Link* link_down,
                                          const simgrid::s4u::Link* backbone)
{
  XBT_DEBUG("Push Host_link for host '%s' to position %lu", netpoint->get_cname(), netpoint->id());
  simgrid::s4u::LinkInRoute linkUp{link_up};
  simgrid::s4u::LinkInRoute linkDown{link_down};
  if (backbone) {
    simgrid::s4u::LinkInRoute linkBB{backbone};
    zone->add_route(netpoint, nullptr, nullptr, nullptr, {linkUp, linkBB}, false);
    zone->add_route(nullptr, netpoint, nullptr, nullptr, {linkBB, linkDown}, false);
  } else {
    zone->add_route(netpoint, nullptr, nullptr, nullptr, {linkUp}, false);
    zone->add_route(nullptr, netpoint, nullptr, nullptr, {linkDown}, false);
  }
}

/** @brief Add a link connecting a host to the rest of its StarZone */
static void sg_platf_build_hostlink(simgrid::kernel::routing::StarZone* zone,
                                    const simgrid::kernel::routing::HostLinkCreationArgs* hostlink,
                                    const simgrid::s4u::Link* backbone)
{
  const auto* engine = simgrid::s4u::Engine::get_instance();
  auto* netpoint     = engine->host_by_name(hostlink->id)->get_netpoint();
  xbt_assert(netpoint, "Host '%s' not found!", hostlink->id.c_str());

  const auto* linkUp   = engine->link_by_name_or_null(hostlink->link_up);
  const auto* linkDown = engine->link_by_name_or_null(hostlink->link_down);

  xbt_assert(linkUp, "Link '%s' not found!", hostlink->link_up.c_str());
  xbt_assert(linkDown, "Link '%s' not found!", hostlink->link_down.c_str());
  sg_platf_cluster_set_hostlink(zone, netpoint, linkUp, linkDown, backbone);
}

/** @brief Create a cabinet (set of hosts) inside a Cluster(StarZone) */
static void sg_platf_build_cabinet(simgrid::kernel::routing::StarZone* zone,
                                   const simgrid::kernel::routing::CabinetCreationArgs* args,
                                   const simgrid::s4u::Link* backbone)
{
  for (int const& radical : args->radicals) {
    std::string id   = args->prefix + std::to_string(radical) + args->suffix;
    auto const* host = zone->create_host(id, {args->speed})->seal();

    const auto* link_up   = zone->create_link("link_" + id + "_UP", {args->bw})->set_latency(args->lat)->seal();
    const auto* link_down = zone->create_link("link_" + id + "_DOWN", {args->bw})->set_latency(args->lat)->seal();

    sg_platf_cluster_set_hostlink(zone, host->get_netpoint(), link_up, link_down, backbone);
  }
}

static void sg_platf_zone_cluster_populate(const simgrid::kernel::routing::ClusterZoneCreationArgs* cluster)
{
  auto* zone = dynamic_cast<simgrid::kernel::routing::StarZone*>(current_routing);
  xbt_assert(zone, "Host_links are only valid for Cluster(Star)");

  const simgrid::s4u::Link* backbone = nullptr;
  /* create backbone */
  if (cluster->backbone) {
    sg_platf_new_link(cluster->backbone.get());
    backbone = simgrid::s4u::Link::by_name(cluster->backbone->id);
  }

  /* create host_links for hosts */
  for (auto const& hostlink : cluster->host_links) {
    sg_platf_build_hostlink(zone, &hostlink, backbone);
  }

  /* create cabinets */
  for (auto const& cabinet : cluster->cabinets) {
    sg_platf_build_cabinet(zone, &cabinet, backbone);
  }
}

void routing_cluster_add_backbone(std::unique_ptr<simgrid::kernel::routing::LinkCreationArgs> link)
{
  zone_cluster.backbone = std::move(link);
}

void sg_platf_new_cabinet(const simgrid::kernel::routing::CabinetCreationArgs* args)
{
  xbt_assert(args, "Invalid nullptr argument");
  zone_cluster.cabinets.emplace_back(*args);
}

/*************************************************************************************************/
void sg_platf_new_route(simgrid::kernel::routing::RouteCreationArgs* route)
{
  current_routing->add_route(route->src, route->dst, route->gw_src, route->gw_dst, route->link_list,
                             route->symmetrical);
}

void sg_platf_new_bypass_route(simgrid::kernel::routing::RouteCreationArgs* route)
{
  current_routing->add_bypass_route(route->src, route->dst, route->gw_src, route->gw_dst, route->link_list);
}

void sg_platf_new_actor(simgrid::kernel::routing::ActorCreationArgs* actor)
{
  const auto* engine = simgrid::s4u::Engine::get_instance();
  sg_host_t host     = sg_host_by_name(actor->host);
  if (not host) {
    // The requested host does not exist. Do a nice message to the user
    std::string msg = std::string("Cannot create actor '") + actor->function + "': host '" + actor->host +
                      "' does not exist\nExisting hosts: '";

    std::vector<simgrid::s4u::Host*> list = engine->get_all_hosts();

    for (auto const& some_host : list) {
      msg += some_host->get_name();
      msg += "', '";
      if (msg.length() > 1024) {
        msg.pop_back(); // remove trailing quote
        msg += "...(list truncated)......";
        break;
      }
    }
    xbt_die("%s", msg.c_str());
  }
  const simgrid::kernel::actor::ActorCodeFactory& factory = engine->get_impl()->get_function(actor->function);
  xbt_assert(factory, "Error while creating an actor from the XML file: Function '%s' not registered", actor->function);

  double start_time = actor->start_time;
  double kill_time  = actor->kill_time;
  bool auto_restart = actor->restart_on_failure;

  std::string actor_name                 = actor->args[0];
  simgrid::kernel::actor::ActorCode code = factory(std::move(actor->args));

  auto* arg = new simgrid::kernel::actor::ProcessArg(actor_name, code, nullptr, host, kill_time, actor->properties,
                                                     auto_restart, /*daemon=*/false, /*restart_count=*/0);

  host->get_impl()->add_actor_at_boot(arg);

  if (start_time > simgrid::s4u::Engine::get_clock()) {
    arg = new simgrid::kernel::actor::ProcessArg(actor_name, code, nullptr, host, kill_time, actor->properties,
                                                 auto_restart, /*daemon=*/false, /*restart_count=*/0);

    XBT_DEBUG("Actor %s@%s will be started at time %f", arg->name.c_str(), arg->host->get_cname(), start_time);
    simgrid::kernel::timer::Timer::set(start_time, [arg]() {
      simgrid::kernel::actor::ActorImplPtr new_actor = simgrid::kernel::actor::ActorImpl::create(arg);
      delete arg;
    });
  } else { // start_time <= simgrid::s4u::Engine::get_clock()
    XBT_DEBUG("Starting actor %s(%s) right now", arg->name.c_str(), host->get_cname());

    try {
      simgrid::kernel::actor::ActorImplPtr new_actor = simgrid::kernel::actor::ActorImpl::create(arg);
    } catch (simgrid::HostFailureException const&) {
      XBT_WARN("Starting actor %s(%s) failed because its host is turned off.", arg->name.c_str(), host->get_cname());
    }
  }
}

/**
 * @brief Auxiliary function to build the object NetZoneImpl
 *
 * Builds the objects, setting its parent properties and root netzone if needed
 * @param zone the parameters defining the Zone to build.
 * @return Pointer to recently created netzone
 */
static simgrid::kernel::routing::NetZoneImpl*
sg_platf_create_zone(const simgrid::kernel::routing::ZoneCreationArgs* zone)
{
  /* search the routing model */
  const simgrid::s4u::NetZone* new_zone = nullptr;

  XBT_DEBUG("Create zone name:%s routing:%s", zone->id.c_str(), zone->routing.c_str());
  simgrid::s4u::NetZone* parent;
  if (current_routing != nullptr)
    parent = current_routing->get_iface();
  else
    parent = simgrid::s4u::Engine::get_instance()->get_netzone_root();

  if (strcasecmp(zone->routing.c_str(), "Cluster") == 0) {
    new_zone = parent->add_netzone_star(zone->id);
  } else if (strcasecmp(zone->routing.c_str(), "Dijkstra") == 0) {
    new_zone = parent->add_netzone_dijkstra(zone->id, false);
  } else if (strcasecmp(zone->routing.c_str(), "DijkstraCache") == 0) {
    new_zone = parent->add_netzone_dijkstra(zone->id, true);
  } else if (strcasecmp(zone->routing.c_str(), "Floyd") == 0) {
    new_zone = parent->add_netzone_floyd(zone->id);
  } else if (strcasecmp(zone->routing.c_str(), "Full") == 0) {
    new_zone = parent->add_netzone_full(zone->id);
  } else if (strcasecmp(zone->routing.c_str(), "None") == 0) {
    new_zone = parent->add_netzone_empty(zone->id);
  } else if (strcasecmp(zone->routing.c_str(), "Vivaldi") == 0) {
    new_zone = parent->add_netzone_vivaldi(zone->id);
  } else if (strcasecmp(zone->routing.c_str(), "Wifi") == 0) {
    new_zone = parent->add_netzone_wifi(zone->id);
  } else {
    xbt_die("Not a valid model!");
  }

  return new_zone->get_impl();
}

/**
 * @brief Add a Zone to the platform
 *
 * Add a new autonomous system to the platform. Any elements (such as host, router or sub-Zone) added after this call
 * and before the corresponding call to sg_platf_new_zone_seal() will be added to this Zone.
 *
 * Once this function was called, the configuration concerning the used models cannot be changed anymore.
 *
 * @param zone the parameters defining the Zone to build.
 */
simgrid::kernel::routing::NetZoneImpl* sg_platf_new_zone_begin(const simgrid::kernel::routing::ZoneCreationArgs* zone)
{
  zone_cluster.routing = zone->routing;
  current_routing      = sg_platf_create_zone(zone);

  return current_routing;
}

void sg_platf_new_zone_set_properties(const std::unordered_map<std::string, std::string>& props)
{
  xbt_assert(current_routing, "Cannot set properties of the current Zone: none under construction");

  current_routing->set_properties(props);
}

/**
 * @brief Specify that the description of the current Zone is finished
 *
 * Once you've declared all the content of your Zone, you have to seal
 * it with this call. Your Zone is not usable until you call this function.
 */
void sg_platf_new_zone_seal()
{
  xbt_assert(current_routing, "Cannot seal the current Zone: none under construction");
  if (strcasecmp(zone_cluster.routing.c_str(), "Cluster") == 0) {
    sg_platf_zone_cluster_populate(&zone_cluster);
    zone_cluster.routing = "";
    zone_cluster.host_links.clear();
    zone_cluster.cabinets.clear();
    zone_cluster.backbone.reset();
  }
  current_routing = current_routing->get_parent();
}

/** @brief Add a link connecting a host to the rest of its Zone (which must be cluster or vivaldi) */
void sg_platf_new_hostlink(const simgrid::kernel::routing::HostLinkCreationArgs* hostlink)
{
  xbt_assert(hostlink, "Invalid nullptr parameter");
  zone_cluster.host_links.emplace_back(*hostlink);
}

void sg_platf_new_trace(const simgrid::kernel::routing::ProfileCreationArgs* args)
{
  simgrid::kernel::profile::Profile* profile;
  if (not args->file.empty()) {
    profile = simgrid::kernel::profile::ProfileBuilder::from_file(args->file);
  } else {
    xbt_assert(not args->pc_data.empty(), "Trace '%s' must have either a content, or point to a file on disk.",
               args->id.c_str());
    profile = simgrid::kernel::profile::ProfileBuilder::from_string(args->id, args->pc_data, args->periodicity);
  }
  traces_set_list.try_emplace(args->id, profile);
}
