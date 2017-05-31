/* Copyright (c) 2008-2017. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/xbt_os_time.h>

#include "simgrid/s4u/Engine.hpp"
#include "simgrid/s4u/Host.hpp"

#include "simgrid/simdag.h"

#include "src/kernel/routing/NetPoint.hpp"
#include "src/surf/network_interface.hpp"


XBT_LOG_NEW_DEFAULT_CATEGORY(flatifier, "Logging specific to this platform parsing tool");

static int name_compare_hosts(const void *n1, const void *n2)
{
  return std::strcmp(sg_host_get_name(*(sg_host_t *) n1), sg_host_get_name(*(sg_host_t *) n2));
}

static int name_compare_links(const void *n1, const void *n2)
{
  return std::strcmp(sg_link_name(*(SD_link_t *) n1),sg_link_name(*(SD_link_t *) n2));
}

static bool parse_cmdline(int* timings, char** platformFile, int argc, char** argv)
{
  bool parse_ok = true;
  for (int i = 1; i < argc; i++) {
    if (std::strlen(argv[i]) > 1 && argv[i][0] == '-' && argv[i][1] == '-') {
      if (not std::strcmp(argv[i], "--timings")) {
        *timings = 1;
      } else {
        parse_ok = false;
        break;
      }
    } else {
      *platformFile = argv[i];
    }
  }
  return parse_ok;
}

static void create_environment(xbt_os_timer_t parse_time, const char *platformFile)
{
  try {
    xbt_os_cputimer_start(parse_time);
    SD_create_environment(platformFile);
    xbt_os_cputimer_stop(parse_time);
  }
  catch (std::exception& e) {
    xbt_die("Error while loading %s: %s", platformFile, e.what());
  }
}

static void dump_platform()
{
  int version = 4;
  xbt_dict_t props = nullptr;
  xbt_dict_cursor_t cursor = nullptr;
  char* key;
  char* data;

  std::printf("<?xml version='1.0'?>\n");
  std::printf("<!DOCTYPE platform SYSTEM \"http://simgrid.gforge.inria.fr/simgrid/simgrid.dtd\">\n");
  std::printf("<platform version=\"%d\">\n", version);
  std::printf("<AS id=\"AS0\" routing=\"Full\">\n");

  // Hosts
  unsigned int totalHosts = sg_host_count();
  sg_host_t* hosts        = sg_host_list();
  std::qsort((void*)hosts, totalHosts, sizeof(sg_host_t), name_compare_hosts);

  for (unsigned int i = 0; i < totalHosts; i++) {
    std::printf("  <host id=\"%s\" speed=\"%.0f\"", hosts[i]->cname(), sg_host_speed(hosts[i]));
    props = sg_host_get_properties(hosts[i]);
    if (hosts[i]->coreCount() > 1) {
      std::printf(" core=\"%d\"", hosts[i]->coreCount());
    }
    if (props && not xbt_dict_is_empty(props)) {
      std::printf(">\n");
      xbt_dict_foreach (props, cursor, key, data) {
        std::printf("    <prop id=\"%s\" value=\"%s\"/>\n", key, data);
      }
      std::printf("  </host>\n");
    } else {
      std::printf("/>\n");
    }
  }

  // Routers
  std::vector<simgrid::kernel::routing::NetPoint*> netcardList;
  simgrid::s4u::Engine::instance()->netpointList(&netcardList);
  std::sort(netcardList.begin(), netcardList.end(),
            [](simgrid::kernel::routing::NetPoint* a, simgrid::kernel::routing::NetPoint* b) {
              return a->name() < b->name();
            });

  for (auto srcCard : netcardList)
    if (srcCard->isRouter())
      std::printf("  <router id=\"%s\"/>\n", srcCard->cname());

  // Links
  unsigned int totalLinks    = sg_link_count();
  simgrid::s4u::Link** links = sg_link_list();

  std::qsort((void*)links, totalLinks, sizeof(SD_link_t), name_compare_links);

  for (unsigned int i = 0; i < totalLinks; i++) {
    simgrid::s4u::Link* link = links[i];
    std::printf("  <link id=\"");

    std::printf("%s\" bandwidth=\"%.0f\" latency=\"%.9f\"", link->name(), link->bandwidth(), link->latency());
    if (sg_link_is_shared(link)) {
      std::printf("/>\n");
    } else {
      std::printf(" sharing_policy=\"FATPIPE\"/>\n");
    }
  }

  for (unsigned int it_src = 0; it_src < totalHosts; it_src++) { // Routes from host
    simgrid::s4u::Host* host1                      = hosts[it_src];
    simgrid::kernel::routing::NetPoint* netcardSrc = host1->pimpl_netpoint;
    for (unsigned int it_dst = 0; it_dst < totalHosts; it_dst++) { // Routes to host
      simgrid::s4u::Host* host2 = hosts[it_dst];
      std::vector<simgrid::surf::LinkImpl*> route;
      simgrid::kernel::routing::NetPoint* netcardDst = host2->pimpl_netpoint;
      simgrid::kernel::routing::NetZoneImpl::getGlobalRoute(netcardSrc, netcardDst, &route, nullptr);
      if (not route.empty()) {
        std::printf("  <route src=\"%s\" dst=\"%s\">\n  ", host1->cname(), host2->cname());
        for (auto link : route)
          std::printf("<link_ctn id=\"%s\"/>", link->cname());
        std::printf("\n  </route>\n");
      }
    }
    for (auto netcardDst : netcardList) { // to router
      if (netcardDst->isRouter()) {
        std::printf("  <route src=\"%s\" dst=\"%s\">\n  ", host1->cname(), netcardDst->cname());
        std::vector<simgrid::surf::LinkImpl*> route;
        simgrid::kernel::routing::NetZoneImpl::getGlobalRoute(netcardSrc, netcardDst, &route, nullptr);
        for (auto link : route)
          std::printf("<link_ctn id=\"%s\"/>", link->cname());
        std::printf("\n  </route>\n");
      }
    }
  }

  for (auto value1 : netcardList) { // Routes from router
    if (value1->isRouter()) {
      for (auto value2 : netcardList) { // to router
        if (value2->isRouter()) {
          std::printf("  <route src=\"%s\" dst=\"%s\">\n  ", value1->cname(), value2->cname());
          std::vector<simgrid::surf::LinkImpl*> route;
          simgrid::kernel::routing::NetZoneImpl::getGlobalRoute(value1, value2, &route, nullptr);
          for (auto link : route)
            std::printf("<link_ctn id=\"%s\"/>", link->cname());
          std::printf("\n  </route>\n");
        }
      }
      for (unsigned int it_dst = 0; it_dst < totalHosts; it_dst++) { // Routes to host
        simgrid::s4u::Host* host2 = hosts[it_dst];
        std::printf("  <route src=\"%s\" dst=\"%s\">\n  ", value1->cname(), host2->cname());
        std::vector<simgrid::surf::LinkImpl*> route;
        simgrid::kernel::routing::NetPoint* netcardDst = host2->pimpl_netpoint;
        simgrid::kernel::routing::NetZoneImpl::getGlobalRoute(value1, netcardDst, &route, nullptr);
        for (auto link : route)
          std::printf("<link_ctn id=\"%s\"/>", link->cname());
        std::printf("\n  </route>\n");
      }
    }
  }

  std::printf("</AS>\n");
  std::printf("</platform>\n");
  std::free(hosts);
  std::free(links);
}

int main(int argc, char** argv)
{
  char* platformFile = nullptr;
  int timings        = 0;

  xbt_os_timer_t parse_time = xbt_os_timer_new();

  SD_init(&argc, argv);

  xbt_assert(parse_cmdline(&timings, &platformFile, argc, argv) && platformFile,
             "Invalid command line arguments: expected [--timings] platformFile");

  XBT_DEBUG("%d,%s", timings, platformFile);

  create_environment(parse_time, platformFile);

  if (timings) {
    XBT_INFO("Parsing time: %fs (%zu hosts, %d links)", xbt_os_timer_elapsed(parse_time), sg_host_count(),
             sg_link_count());
  } else {
    dump_platform();
  }

  xbt_os_timer_free(parse_time);

  return 0;
}
