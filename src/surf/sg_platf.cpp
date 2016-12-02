/* Copyright (c) 2006-2014. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "xbt/misc.h"
#include "xbt/log.h"
#include "xbt/str.h"
#include "xbt/dict.h"
#include "xbt/RngStream.h"
#include <xbt/functional.hpp>
#include <xbt/signal.hpp>
#include "src/surf/HostImpl.hpp"
#include "surf/surf.h"

#include "src/simix/smx_private.h"

#include "src/include/simgrid/sg_config.h"
#include "src/surf/xml/platf_private.hpp"

#include "src/surf/HostImpl.hpp"
#include "src/surf/cpu_interface.hpp"
#include "src/surf/network_interface.hpp"
#include "surf/surf_routing.h" // FIXME: brain dead public header

#include "src/kernel/routing/AsImpl.hpp"
#include "src/kernel/routing/AsCluster.hpp"
#include "src/kernel/routing/AsClusterTorus.hpp"
#include "src/kernel/routing/AsClusterFatTree.hpp"
#include "src/kernel/routing/AsClusterDragonfly.hpp"
#include "src/kernel/routing/AsDijkstra.hpp"
#include "src/kernel/routing/AsFloyd.hpp"
#include "src/kernel/routing/AsFull.hpp"
#include "src/kernel/routing/AsNone.hpp"
#include "src/kernel/routing/AsVivaldi.hpp"

XBT_LOG_EXTERNAL_DEFAULT_CATEGORY(surf_parse);

XBT_PRIVATE xbt_dynar_t mount_list = nullptr;

namespace simgrid {
namespace surf {

simgrid::xbt::signal<void(sg_platf_link_cbarg_t)> on_link;
simgrid::xbt::signal<void(sg_platf_cluster_cbarg_t)> on_cluster;
simgrid::xbt::signal<void(void)> on_postparse;

}
}

static int surf_parse_models_setup_already_called = 0;

/** The current AS in the parsing */
static simgrid::kernel::routing::AsImpl *current_routing = nullptr;
static simgrid::kernel::routing::AsImpl *routing_get_current()
{
  return current_routing;
}

/** Module management function: creates all internal data structures */
void sg_platf_init() {
}

/** Module management function: frees all internal data structures */
void sg_platf_exit() {
  simgrid::surf::on_link.disconnect_all_slots();
  simgrid::surf::on_cluster.disconnect_all_slots();
  simgrid::surf::on_postparse.disconnect_all_slots();

  /* make sure that we will reinit the models while loading the platf once reinited */
  surf_parse_models_setup_already_called = 0;
  surf_parse_lex_destroy();
}

/** @brief Add an host to the current AS */
void sg_platf_new_host(sg_platf_host_cbarg_t args)
{
  simgrid::s4u::Host* host = routing_get_current()->createHost(args->id, &args->speed_per_pstate, args->core_amount);

  new simgrid::surf::HostImpl(host, mount_list);
  xbt_lib_set(storage_lib, args->id, ROUTING_STORAGE_HOST_LEVEL, static_cast<void*>(mount_list));
  mount_list = nullptr;

  if (args->properties) {
    xbt_dict_cursor_t cursor=nullptr;
    char *key,*data;
    xbt_dict_foreach (args->properties, cursor, key, data)
      host->setProperty(key, data);
    xbt_dict_free(&args->properties);
  }

  /* Change from the defaults */
  if (args->state_trace)
    host->pimpl_cpu->setStateTrace(args->state_trace);
  if (args->speed_trace)
    host->pimpl_cpu->setSpeedTrace(args->speed_trace);
  if (args->pstate != 0)
    host->pimpl_cpu->setPState(args->pstate);
  if (args->coord && strcmp(args->coord, ""))
    new simgrid::kernel::routing::vivaldi::Coords(host, args->coord);

  simgrid::s4u::Host::onCreation(*host);

  if (TRACE_is_enabled() && TRACE_needs_platform())
    sg_instr_new_host(*host);
}

/** @brief Add a "router" to the network element list */
void sg_platf_new_router(sg_platf_router_cbarg_t router)
{
  using simgrid::kernel::routing::AsCluster;
  simgrid::kernel::routing::AsImpl* current_routing = routing_get_current();

  if (current_routing->hierarchy_ == simgrid::kernel::routing::AsImpl::RoutingMode::unset)
    current_routing->hierarchy_ = simgrid::kernel::routing::AsImpl::RoutingMode::base;
  xbt_assert(nullptr == xbt_lib_get_or_null(as_router_lib, router->id, ROUTING_ASR_LEVEL),
             "Refusing to create a router named '%s': this name already describes a node.", router->id);

  simgrid::kernel::routing::NetCard* netcard =
    new simgrid::kernel::routing::NetCardImpl(router->id, simgrid::kernel::routing::NetCard::Type::Router, current_routing);
  xbt_lib_set(as_router_lib, router->id, ROUTING_ASR_LEVEL, netcard);
  XBT_DEBUG("Router '%s' has the id %d", router->id, netcard->id());

  if (router->coord && strcmp(router->coord, "")) {
    unsigned int cursor;
    char*str;

    xbt_assert(COORD_ASR_LEVEL, "To use host coordinates, please add --cfg=network/coordinates:yes to your command line");
    /* Pre-parse the host coordinates */
    xbt_dynar_t ctn_str = xbt_str_split_str(router->coord, " ");
    xbt_assert(xbt_dynar_length(ctn_str)==3,"Coordinates of %s must have 3 dimensions", router->id);
    xbt_dynar_t ctn = xbt_dynar_new(sizeof(double),nullptr);
    xbt_dynar_foreach(ctn_str,cursor, str) {
      double val = xbt_str_parse_double(str, "Invalid coordinate: %s");
      xbt_dynar_push(ctn,&val);
    }
    xbt_dynar_free(&ctn_str);
    xbt_dynar_shrink(ctn, 0);
    xbt_lib_set(as_router_lib, router->id, COORD_ASR_LEVEL, (void *) ctn);
  }

  auto cluster = dynamic_cast<AsCluster*>(current_routing);
  if(cluster != nullptr)
    cluster->router_ = static_cast<simgrid::kernel::routing::NetCard*>(xbt_lib_get_or_null(as_router_lib, router->id, ROUTING_ASR_LEVEL));

  if (TRACE_is_enabled() && TRACE_needs_platform())
    sg_instr_new_router(router);
}

void sg_platf_new_link(sg_platf_link_cbarg_t link){
  std::vector<char*> names;

  if (link->policy == SURF_LINK_FULLDUPLEX) {
    names.push_back(bprintf("%s_UP", link->id));
    names.push_back(bprintf("%s_DOWN", link->id));
  } else {
    names.push_back(xbt_strdup(link->id));
  }
  for (auto link_name : names) {
    Link* l = surf_network_model->createLink(link_name, link->bandwidth, link->latency, link->policy);

    if (link->properties) {
      xbt_dict_cursor_t cursor = nullptr;
      char *key, *data;
      xbt_dict_foreach (link->properties, cursor, key, data)
        l->setProperty(key, data);
      xbt_dict_free(&link->properties);
    }

    if (link->latency_trace)
      l->setLatencyTrace(link->latency_trace);
    if (link->bandwidth_trace)
      l->setBandwidthTrace(link->bandwidth_trace);
    if (link->state_trace)
      l->setStateTrace(link->state_trace);

    xbt_free(link_name);
  }

  simgrid::surf::on_link(link);
}

void sg_platf_new_cluster(sg_platf_cluster_cbarg_t cluster)
{
  using simgrid::kernel::routing::AsCluster;
  using simgrid::kernel::routing::AsClusterDragonfly;
  using simgrid::kernel::routing::AsClusterFatTree;
  using simgrid::kernel::routing::AsClusterTorus;

  int rankId=0;

  s_sg_platf_link_cbarg_t link;

  // What an inventive way of initializing the AS that I have as ancestor :-(
  s_sg_platf_AS_cbarg_t AS;
  AS.id = cluster->id;
  switch (cluster->topology) {
  case SURF_CLUSTER_TORUS:
    AS.routing = A_surfxml_AS_routing_ClusterTorus;
    break;
  case SURF_CLUSTER_DRAGONFLY:
    AS.routing = A_surfxml_AS_routing_ClusterDragonfly;
    break;
  case SURF_CLUSTER_FAT_TREE:
    AS.routing = A_surfxml_AS_routing_ClusterFatTree;
    break;
  default:
    AS.routing = A_surfxml_AS_routing_Cluster;
    break;
  }
  sg_platf_new_AS_begin(&AS);
  simgrid::kernel::routing::AsCluster *current_as = static_cast<AsCluster*>(routing_get_current());
  current_as->parse_specific_arguments(cluster);

  if(cluster->loopback_bw!=0 || cluster->loopback_lat!=0){
    current_as->linkCountPerNode_++;
    current_as->hasLoopback_ = 1;
  }

  if(cluster->limiter_link!=0){
    current_as->linkCountPerNode_++;
    current_as->hasLimiter_ = 1;
  }

  for (int i : *cluster->radicals) {
    char * host_id = bprintf("%s%d%s", cluster->prefix, i, cluster->suffix);
    char * link_id = bprintf("%s_link_%d", cluster->id, i);

    XBT_DEBUG("<host\tid=\"%s\"\tpower=\"%f\">", host_id, cluster->speed);

    s_sg_platf_host_cbarg_t host;
    memset(&host, 0, sizeof(host));
    host.id = host_id;
    if ((cluster->properties != nullptr) && (!xbt_dict_is_empty(cluster->properties))) {
      xbt_dict_cursor_t cursor=nullptr;
      char *key,*data;
      host.properties = xbt_dict_new();

      xbt_dict_foreach(cluster->properties,cursor,key,data) {
        xbt_dict_set(host.properties, key, xbt_strdup(data),free);
      }
    }

    host.speed_per_pstate.push_back(cluster->speed);
    host.pstate = 0;
    host.core_amount = cluster->core_amount;
    host.coord = "";
    sg_platf_new_host(&host);
    XBT_DEBUG("</host>");

    XBT_DEBUG("<link\tid=\"%s\"\tbw=\"%f\"\tlat=\"%f\"/>", link_id, cluster->bw, cluster->lat);

    s_surf_parsing_link_up_down_t info_lim;
    s_surf_parsing_link_up_down_t info_loop;
    // All links are saved in a matrix;
    // every row describes a single node; every node may have multiple links.
    // the first column may store a link from x to x if p_has_loopback is set
    // the second column may store a limiter link if p_has_limiter is set
    // other columns are to store one or more link for the node

    //add a loopback link
    if(cluster->loopback_bw!=0 || cluster->loopback_lat!=0){
      char *tmp_link = bprintf("%s_loopback", link_id);
      XBT_DEBUG("<loopback\tid=\"%s\"\tbw=\"%f\"/>", tmp_link, cluster->loopback_bw);

      memset(&link, 0, sizeof(link));
      link.id        = tmp_link;
      link.bandwidth = cluster->loopback_bw;
      link.latency   = cluster->loopback_lat;
      link.policy    = SURF_LINK_FATPIPE;
      sg_platf_new_link(&link);
      info_loop.linkUp = Link::byName(tmp_link);
      info_loop.linkDown = Link::byName(tmp_link);
      free(tmp_link);

      auto as_cluster = static_cast<AsCluster*>(current_as);
      as_cluster->privateLinks_.insert({rankId*as_cluster->linkCountPerNode_, info_loop});
    }

    //add a limiter link (shared link to account for maximal bandwidth of the node)
    if(cluster->limiter_link!=0){
      char *tmp_link = bprintf("%s_limiter", link_id);
      XBT_DEBUG("<limiter\tid=\"%s\"\tbw=\"%f\"/>", tmp_link, cluster->limiter_link);

      memset(&link, 0, sizeof(link));
      link.id = tmp_link;
      link.bandwidth = cluster->limiter_link;
      link.latency = 0;
      link.policy = SURF_LINK_SHARED;
      sg_platf_new_link(&link);
      info_lim.linkUp = info_lim.linkDown = Link::byName(tmp_link);
      free(tmp_link);
      current_as->privateLinks_.insert(
          {rankId * current_as->linkCountPerNode_ + current_as->hasLoopback_ , info_lim});
    }

    //call the cluster function that adds the others links
    if (cluster->topology == SURF_CLUSTER_FAT_TREE) {
      static_cast<AsClusterFatTree*>(current_as)->addProcessingNode(i);
    }
    else {
      current_as->create_links_for_node(cluster, i, rankId,
          rankId*current_as->linkCountPerNode_ + current_as->hasLoopback_ + current_as->hasLimiter_ );
    }
    xbt_free(link_id);
    xbt_free(host_id);
    rankId++;
  }

  // Add a router. It is magically used thanks to the way in which surf_routing_cluster is written,
  // and it's very useful to connect clusters together
  XBT_DEBUG(" ");
  XBT_DEBUG("<router id=\"%s\"/>", cluster->router_id);
  char *newid = nullptr;
  s_sg_platf_router_cbarg_t router;
  memset(&router, 0, sizeof(router));
  router.id = cluster->router_id;
  if (!router.id || !strcmp(router.id, ""))
    router.id = newid = bprintf("%s%s_router%s", cluster->prefix, cluster->id, cluster->suffix);
  sg_platf_new_router(&router);
  current_as->router_ = (simgrid::kernel::routing::NetCard*) xbt_lib_get_or_null(as_router_lib, router.id, ROUTING_ASR_LEVEL);
  free(newid);

  //Make the backbone
  if ((cluster->bb_bw != 0) || (cluster->bb_lat != 0)) {

    memset(&link, 0, sizeof(link));
    link.id        = bprintf("%s_backbone", cluster->id);
    link.bandwidth = cluster->bb_bw;
    link.latency   = cluster->bb_lat;
    link.policy    = cluster->bb_sharing_policy;

    XBT_DEBUG("<link\tid=\"%s\" bw=\"%f\" lat=\"%f\"/>", link.id, cluster->bb_bw, cluster->bb_lat);
    sg_platf_new_link(&link);

    routing_cluster_add_backbone(Link::byName(link.id));
    free((char*)link.id);
  }

  XBT_DEBUG("</AS>");
  sg_platf_new_AS_seal();

  simgrid::surf::on_cluster(cluster);
  delete cluster->radicals;
}
void routing_cluster_add_backbone(simgrid::surf::Link* bb) {
  simgrid::kernel::routing::AsCluster *cluster = dynamic_cast<simgrid::kernel::routing::AsCluster*>(current_routing);

  xbt_assert(cluster, "Only hosts from Cluster can get a backbone.");
  xbt_assert(nullptr == cluster->backbone_, "Cluster %s already has a backbone link!", cluster->name());

  cluster->backbone_ = bb;
  XBT_DEBUG("Add a backbone to AS '%s'", current_routing->name());
}

void sg_platf_new_cabinet(sg_platf_cabinet_cbarg_t cabinet)
{
  for (int radical : *cabinet->radicals) {
    char *hostname = bprintf("%s%d%s", cabinet->prefix, radical, cabinet->suffix);
    s_sg_platf_host_cbarg_t host;
    memset(&host, 0, sizeof(host));
    host.pstate           = 0;
    host.core_amount      = 1;
    host.id               = hostname;
    host.speed_per_pstate.push_back(cabinet->speed);
    sg_platf_new_host(&host);

    s_sg_platf_link_cbarg_t link;
    memset(&link, 0, sizeof(link));
    link.policy    = SURF_LINK_FULLDUPLEX;
    link.latency   = cabinet->lat;
    link.bandwidth = cabinet->bw;
    link.id        = bprintf("link_%s",hostname);
    sg_platf_new_link(&link);
    free((char*)link.id);

    s_sg_platf_host_link_cbarg_t host_link;
    memset(&host_link, 0, sizeof(host_link));
    host_link.id        = hostname;
    host_link.link_up   = bprintf("link_%s_UP",hostname);
    host_link.link_down = bprintf("link_%s_DOWN",hostname);
    sg_platf_new_hostlink(&host_link);
    free((char*)host_link.link_up);
    free((char*)host_link.link_down);

    free(hostname);
  }
  delete cabinet->radicals;
}

void sg_platf_new_storage(sg_platf_storage_cbarg_t storage)
{
  xbt_assert(!xbt_lib_get_or_null(storage_lib, storage->id,ROUTING_STORAGE_LEVEL),
               "Refusing to add a second storage named \"%s\"", storage->id);

  void* stype = xbt_lib_get_or_null(storage_type_lib, storage->type_id,ROUTING_STORAGE_TYPE_LEVEL);
  xbt_assert(stype,"No storage type '%s'", storage->type_id);

  XBT_DEBUG("ROUTING Create a storage name '%s' with type_id '%s' and content '%s'",
      storage->id,
      storage->type_id,
      storage->content);

  xbt_lib_set(storage_lib, storage->id, ROUTING_STORAGE_LEVEL, (void *) xbt_strdup(storage->type_id));

  // if storage content is not specified use the content of storage_type if any
  if(!strcmp(storage->content,"") && strcmp(((storage_type_t) stype)->content,"")){
    storage->content = ((storage_type_t) stype)->content;
    storage->content_type = ((storage_type_t) stype)->content_type;
    XBT_DEBUG("For disk '%s' content is empty, inherit the content (of type %s) from storage type '%s' ",
        storage->id,((storage_type_t) stype)->content_type,
        ((storage_type_t) stype)->type_id);
  }

  XBT_DEBUG("SURF storage create resource\n\t\tid '%s'\n\t\ttype '%s' "
      "\n\t\tmodel '%s' \n\t\tcontent '%s'\n\t\tcontent_type '%s' "
      "\n\t\tproperties '%p''\n",
      storage->id,
      ((storage_type_t) stype)->model,
      ((storage_type_t) stype)->type_id,
      storage->content,
      storage->content_type,
    storage->properties);

  auto s = surf_storage_model->createStorage(storage->id, ((storage_type_t)stype)->type_id, storage->content,
                                             storage->content_type, storage->attach);

  if (storage->properties) {
    xbt_dict_cursor_t cursor = nullptr;
    char *key, *data;
    xbt_dict_foreach (storage->properties, cursor, key, data)
      s->setProperty(key, data);
    xbt_dict_free(&storage->properties);
  }
}
void sg_platf_new_storage_type(sg_platf_storage_type_cbarg_t storage_type){

  xbt_assert(!xbt_lib_get_or_null(storage_type_lib, storage_type->id,ROUTING_STORAGE_TYPE_LEVEL),
               "Reading a storage type, processing unit \"%s\" already exists", storage_type->id);

  storage_type_t stype = xbt_new0(s_storage_type_t, 1);
  stype->model = xbt_strdup(storage_type->model);
  stype->properties = storage_type->properties;
  stype->content = xbt_strdup(storage_type->content);
  stype->content_type = xbt_strdup(storage_type->content_type);
  stype->type_id = xbt_strdup(storage_type->id);
  stype->size = storage_type->size;
  stype->model_properties = storage_type->model_properties;

  XBT_DEBUG("ROUTING Create a storage type id '%s' with model '%s', "
      "content '%s', and content_type '%s'",
      stype->type_id,
      stype->model,
      storage_type->content,
      storage_type->content_type);

  xbt_lib_set(storage_type_lib,
      stype->type_id,
      ROUTING_STORAGE_TYPE_LEVEL,
      (void *) stype);
}

static void mount_free(void *p)
{
  mount_t mnt = (mount_t) p;
  xbt_free(mnt->name);
}

void sg_platf_new_mount(sg_platf_mount_cbarg_t mount){
  xbt_assert(xbt_lib_get_or_null(storage_lib, mount->storageId, ROUTING_STORAGE_LEVEL),
      "Cannot mount non-existent disk \"%s\"", mount->storageId);

  XBT_DEBUG("ROUTING Mount '%s' on '%s'",mount->storageId, mount->name);

  s_mount_t mnt;
  mnt.storage = surf_storage_resource_priv(surf_storage_resource_by_name(mount->storageId));
  mnt.name = xbt_strdup(mount->name);

  if(!mount_list){
    XBT_DEBUG("Create a Mount list for %s",A_surfxml_host_id);
    mount_list = xbt_dynar_new(sizeof(s_mount_t), mount_free);
  }
  xbt_dynar_push(mount_list, &mnt);
}

void sg_platf_new_route(sg_platf_route_cbarg_t route)
{
  routing_get_current()->addRoute(route);
}

void sg_platf_new_bypassRoute(sg_platf_route_cbarg_t bypassRoute)
{
  routing_get_current()->addBypassRoute(bypassRoute);
}

void sg_platf_new_process(sg_platf_process_cbarg_t process)
{
  sg_host_t host = sg_host_by_name(process->host);
  if (!host) {
    // The requested host does not exist. Do a nice message to the user
    char *tmp = bprintf("Cannot create process '%s': host '%s' does not exist\nExisting hosts: '",process->function, process->host);
    xbt_strbuff_t msg = xbt_strbuff_new_from(tmp);
    free(tmp);
    xbt_dynar_t all_hosts = xbt_dynar_sort_strings(sg_hosts_as_dynar());
    simgrid::s4u::Host* host;
    unsigned int cursor;
    xbt_dynar_foreach(all_hosts,cursor, host) {
      xbt_strbuff_append(msg,host->name().c_str());
      xbt_strbuff_append(msg,"', '");
      if (msg->used > 1024) {
        msg->data[msg->used-3]='\0';
        msg->used -= 3;

        xbt_strbuff_append(msg," ...(list truncated)......");// That will be shortened by 3 chars when existing the loop
        break;
      }
    }
    msg->data[msg->used-3]='\0';
    xbt_die("%s", msg->data);
  }
  simgrid::simix::ActorCodeFactory& factory = SIMIX_get_actor_code_factory(process->function);
  xbt_assert(factory, "Function '%s' unknown", process->function);

  double start_time = process->start_time;
  double kill_time  = process->kill_time;
  int auto_restart = process->on_failure == SURF_PROCESS_ON_FAILURE_DIE ? 0 : 1;

  std::vector<std::string> args(process->argv, process->argv + process->argc);
  std::function<void()> code = factory(std::move(args));

  smx_process_arg_t arg = nullptr;
  smx_actor_t process_created = nullptr;

  arg = new simgrid::simix::ProcessArg();
  arg->name = std::string(process->argv[0]);
  arg->code = code;
  arg->data = nullptr;
  arg->host = host;
  arg->kill_time = kill_time;
  arg->properties = current_property_set;

  sg_host_simix(host)->boot_processes.push_back(arg);

  if (start_time > SIMIX_get_clock()) {

    arg = new simgrid::simix::ProcessArg();
    arg->name = std::string(process->argv[0]);
    arg->code = std::move(code);
    arg->data = nullptr;
    arg->host = host;
    arg->kill_time = kill_time;
    arg->properties = current_property_set;

    XBT_DEBUG("Process %s@%s will be started at time %f",
      arg->name.c_str(), arg->host->name().c_str(), start_time);
    SIMIX_timer_set(start_time, [=]() {
      simix_global->create_process_function(
                                            arg->name.c_str(),
                                            std::move(arg->code),
                                            arg->data,
                                            arg->host,
                                            arg->kill_time,
                                            arg->properties,
                                            arg->auto_restart,
                                            nullptr);
      delete arg;
    });
  } else {                      // start_time <= SIMIX_get_clock()
    XBT_DEBUG("Starting Process %s(%s) right now", arg->name.c_str(), host->cname());

    process_created = simix_global->create_process_function(
        arg->name.c_str(), std::move(code), nullptr,
        host, kill_time,
        current_property_set, auto_restart, nullptr);

    /* verify if process has been created (won't be the case if the host is currently dead, but that's fine) */
    if (!process_created) {
      return;
    }
  }
  current_property_set = nullptr;
}

void sg_platf_new_peer(sg_platf_peer_cbarg_t peer)
{
  using simgrid::kernel::routing::NetCard;
  using simgrid::kernel::routing::AsCluster;

  char *host_id = bprintf("peer_%s", peer->id);
  char *router_id = bprintf("router_%s", peer->id);

  XBT_DEBUG(" ");

  XBT_DEBUG("<AS id=\"%s\"\trouting=\"Cluster\">", peer->id);
  s_sg_platf_AS_cbarg_t AS;
  AS.id      = peer->id;
  AS.routing = A_surfxml_AS_routing_Cluster;
  sg_platf_new_AS_begin(&AS);

  XBT_DEBUG("<host\tid=\"%s\"\tpower=\"%f\"/>", host_id, peer->speed);
  s_sg_platf_host_cbarg_t host;
  memset(&host, 0, sizeof(host));
  host.id = host_id;

  host.speed_per_pstate.push_back(peer->speed);
  host.pstate = 0;
  host.speed_trace = peer->availability_trace;
  host.state_trace = peer->state_trace;
  host.core_amount = 1;
  sg_platf_new_host(&host);

  s_sg_platf_link_cbarg_t link;
  memset(&link, 0, sizeof(link));
  link.policy  = SURF_LINK_SHARED;
  link.latency = peer->lat;

  char* link_up = bprintf("link_%s_UP",peer->id);
  XBT_DEBUG("<link\tid=\"%s\"\tbw=\"%f\"\tlat=\"%f\"/>", link_up, peer->bw_out, peer->lat);
  link.id = link_up;
  link.bandwidth = peer->bw_out;
  sg_platf_new_link(&link);

  char* link_down = bprintf("link_%s_DOWN",peer->id);
  XBT_DEBUG("<link\tid=\"%s\"\tbw=\"%f\"\tlat=\"%f\"/>", link_down, peer->bw_in, peer->lat);
  link.id = link_down;
  link.bandwidth = peer->bw_in;
  sg_platf_new_link(&link);

  XBT_DEBUG("<host_link\tid=\"%s\"\tup=\"%s\"\tdown=\"%s\" />", host_id,link_up,link_down);
  s_sg_platf_host_link_cbarg_t host_link;
  memset(&host_link, 0, sizeof(host_link));
  host_link.id        = host_id;
  host_link.link_up   = link_up;
  host_link.link_down = link_down;
  sg_platf_new_hostlink(&host_link);
  free(link_up);
  free(link_down);

  XBT_DEBUG("<router id=\"%s\"/>", router_id);
  s_sg_platf_router_cbarg_t router;
  memset(&router, 0, sizeof(router));
  router.id = router_id;
  router.coord = peer->coord;
  sg_platf_new_router(&router);

  XBT_DEBUG("</AS>");
  sg_platf_new_AS_seal();
  XBT_DEBUG(" ");

  free(router_id);
  free(host_id);
}

void sg_platf_begin() { /* Do nothing: just for symmetry of user code */ }

void sg_platf_end() {
  simgrid::surf::on_postparse();
}

/* Pick the right models for CPU, net and host, and call their model_init_preparse */
static void surf_config_models_setup()
{
  const char* host_model_name    = xbt_cfg_get_string("host/model");
  const char* vm_model_name      = xbt_cfg_get_string("vm/model");
  const char* network_model_name = xbt_cfg_get_string("network/model");
  const char* cpu_model_name     = xbt_cfg_get_string("cpu/model");
  const char* storage_model_name = xbt_cfg_get_string("storage/model");

  /* The compound host model is needed when using non-default net/cpu models */
  if ((!xbt_cfg_is_default_value("network/model") || !xbt_cfg_is_default_value("cpu/model")) &&
      xbt_cfg_is_default_value("host/model")) {
    host_model_name = "compound";
    xbt_cfg_set_string("host/model", host_model_name);
  }

  XBT_DEBUG("host model: %s", host_model_name);
  if (!strcmp(host_model_name, "compound")) {
    xbt_assert(cpu_model_name, "Set a cpu model to use with the 'compound' host model");
    xbt_assert(network_model_name, "Set a network model to use with the 'compound' host model");

    int cpu_id = find_model_description(surf_cpu_model_description, cpu_model_name);
    surf_cpu_model_description[cpu_id].model_init_preparse();

    int network_id = find_model_description(surf_network_model_description, network_model_name);
    surf_network_model_description[network_id].model_init_preparse();
  }

  XBT_DEBUG("Call host_model_init");
  int host_id = find_model_description(surf_host_model_description, host_model_name);
  surf_host_model_description[host_id].model_init_preparse();

  XBT_DEBUG("Call vm_model_init");
  int vm_id = find_model_description(surf_vm_model_description, vm_model_name);
  surf_vm_model_description[vm_id].model_init_preparse();

  XBT_DEBUG("Call storage_model_init");
  int storage_id = find_model_description(surf_storage_model_description, storage_model_name);
  surf_storage_model_description[storage_id].model_init_preparse();
}

/**
 * \brief Add an AS to the platform
 *
 * Add a new autonomous system to the platform. Any elements (such as host,
 * router or sub-AS) added after this call and before the corresponding call
 * to sg_platf_new_AS_seal() will be added to this AS.
 *
 * Once this function was called, the configuration concerning the used
 * models cannot be changed anymore.
 *
 * @param AS the parameters defining the AS to build.
 */
simgrid::s4u::As * sg_platf_new_AS_begin(sg_platf_AS_cbarg_t AS)
{
  if (!surf_parse_models_setup_already_called) {
    /* Initialize the surf models. That must be done after we got all config, and before we need the models.
     * That is, after the last <config> tag, if any, and before the first of cluster|peer|AS|trace|trace_connect
     *
     * I'm not sure for <trace> and <trace_connect>, there may be a bug here
     * (FIXME: check it out by creating a file beginning with one of these tags)
     * but cluster and peer create ASes internally, so putting the code in there is ok.
     */
    surf_parse_models_setup_already_called = 1;
    surf_config_models_setup();
  }

  _sg_cfg_init_status = 2; /* HACK: direct access to the global controlling the level of configuration to prevent
                            * any further config now that we created some real content */


  /* search the routing model */
  simgrid::kernel::routing::AsImpl *new_as = nullptr;
  switch(AS->routing){
    case A_surfxml_AS_routing_Cluster:
      new_as = new simgrid::kernel::routing::AsCluster(current_routing, AS->id);
      break;
    case A_surfxml_AS_routing_ClusterDragonfly:
      new_as = new simgrid::kernel::routing::AsClusterDragonfly(current_routing, AS->id);
      break;
    case A_surfxml_AS_routing_ClusterTorus:
      new_as = new simgrid::kernel::routing::AsClusterTorus(current_routing, AS->id);
      break;
    case A_surfxml_AS_routing_ClusterFatTree:
      new_as = new simgrid::kernel::routing::AsClusterFatTree(current_routing, AS->id);
      break;
    case A_surfxml_AS_routing_Dijkstra:
      new_as = new simgrid::kernel::routing::AsDijkstra(current_routing, AS->id, 0);
      break;
    case A_surfxml_AS_routing_DijkstraCache:
      new_as = new simgrid::kernel::routing::AsDijkstra(current_routing, AS->id, 1);
      break;
    case A_surfxml_AS_routing_Floyd:
      new_as = new simgrid::kernel::routing::AsFloyd(current_routing, AS->id);
      break;
    case A_surfxml_AS_routing_Full:
      new_as = new simgrid::kernel::routing::AsFull(current_routing, AS->id);
      break;
    case A_surfxml_AS_routing_None:
      new_as = new simgrid::kernel::routing::AsNone(current_routing, AS->id);
      break;
    case A_surfxml_AS_routing_Vivaldi:
      new_as = new simgrid::kernel::routing::AsVivaldi(current_routing, AS->id);
      break;
    default:
      xbt_die("Not a valid model!");
      break;
  }

  if (current_routing == nullptr) { /* it is the first one */
    xbt_assert(routing_platf->root_ == nullptr, "All defined components must belong to a AS");
    routing_platf->root_ = new_as;

  } else {
    /* set the father behavior */
    if (current_routing->hierarchy_ == simgrid::kernel::routing::AsImpl::RoutingMode::unset)
      current_routing->hierarchy_ = simgrid::kernel::routing::AsImpl::RoutingMode::recursive;
    /* add to the sons dictionary */
    xbt_dict_set(current_routing->children(), AS->id, (void *) new_as, nullptr);
  }

  /* set the new current component of the tree */
  current_routing = new_as;

  simgrid::kernel::routing::asCreatedCallbacks(new_as);
  if (TRACE_is_enabled())
    sg_instr_AS_begin(AS);

  return new_as;
}

/**
 * \brief Specify that the description of the current AS is finished
 *
 * Once you've declared all the content of your AS, you have to seal
 * it with this call. Your AS is not usable until you call this function.
 */
void sg_platf_new_AS_seal()
{
  xbt_assert(current_routing, "Cannot seal the current AS: none under construction");
  current_routing->seal();
  current_routing = static_cast<simgrid::kernel::routing::AsImpl*>(current_routing->father());

  if (TRACE_is_enabled())
    sg_instr_AS_end();
}

/** @brief Add a link connecting an host to the rest of its AS (which must be cluster or vivaldi) */
void sg_platf_new_hostlink(sg_platf_host_link_cbarg_t hostlink)
{
  simgrid::kernel::routing::NetCard *netcard = sg_host_by_name(hostlink->id)->pimpl_netcard;
  xbt_assert(netcard, "Host '%s' not found!", hostlink->id);
  xbt_assert(dynamic_cast<simgrid::kernel::routing::AsCluster*>(current_routing),
      "Only hosts from Cluster and Vivaldi ASes can get an host_link.");

  s_surf_parsing_link_up_down_t link_up_down;
  link_up_down.linkUp = Link::byName(hostlink->link_up);
  link_up_down.linkDown = Link::byName(hostlink->link_down);

  xbt_assert(link_up_down.linkUp, "Link '%s' not found!",hostlink->link_up);
  xbt_assert(link_up_down.linkDown, "Link '%s' not found!",hostlink->link_down);

  auto as_cluster = static_cast<simgrid::kernel::routing::AsCluster*>(current_routing);

  if (as_cluster->privateLinks_.find(netcard->id()) != as_cluster->privateLinks_.end())
    surf_parse_error("Host_link for '%s' is already defined!",hostlink->id);

  XBT_DEBUG("Push Host_link for host '%s' to position %d", netcard->name().c_str(), netcard->id());
  as_cluster->privateLinks_.insert({netcard->id(), link_up_down});
}
