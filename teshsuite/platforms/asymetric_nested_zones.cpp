/* Copyright (c) 2006-2023. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */
/*
                                  rootzone
                                     |
                                  rootlink
                                     |
               +---------------------+----------------------+
               |                     |                      |
              zone_A               zone_B                 zone_C
              /    \                |                     /  \
            A1     A2              B1                   h1    h2
           / \     \                |
         h1  h2    h1              h1

*/

#include "simgrid/s4u/Link.hpp"
#include "simgrid/s4u/NetZone.hpp"
#include <simgrid/kernel/routing/NetPoint.hpp>
#include <simgrid/s4u.hpp>

#define LIRDUP sg4::LinkInRoute::Direction::UP
namespace sg4 = simgrid::s4u;

static sg4::NetZone* create_cluster(const sg4::NetZone* root, const std::string& cluster_name,
                                    const std::vector<std::string>& hosts)
{
  simgrid::s4u::NetZone* cluster = sg4::create_star_zone(cluster_name)->set_parent(root);
  /* create the backbone link */
  const sg4::Link* l_bb = cluster->create_link("backbonelink" + cluster_name, "20Gbps")->set_latency("500us");
  /* create all hosts and connect them to outside world */
  for (const std::string& hostname : hosts) {
    /* create host */
    sg4::Host* host = cluster->create_host(hostname, "1Gf");

    /* create link */
    const sg4::Link* l = cluster->create_link(hostname + "link", "1Gbps")->set_latency("100us");

    /* create route */
    sg4::LinkInRoute backbone{l_bb, sg4::LinkInRoute::Direction::UP};
    sg4::LinkInRoute linkroute{l, sg4::LinkInRoute::Direction::UP};
    cluster->add_route(host, nullptr, {linkroute, backbone}, true);
  }
  /* create gateway */
  cluster->set_gateway(cluster->create_router(cluster_name + "_router"));
  cluster->seal();
  return cluster;
}

extern "C" void load_platform(const sg4::Engine& e);
void load_platform(const sg4::Engine& e)
{
  simgrid::s4u::NetZone* root = sg4::create_star_zone("rootzone");

  /* create zone_A */
  simgrid::s4u::NetZone* zone_a = sg4::create_star_zone("zone_A");
  zone_a->set_parent(root);
  zone_a->set_gateway(zone_a->create_router(std::string("zone_A") + "_router"));
  /* create cluster_A1 */
  simgrid::s4u::NetZone* clusterA1 = create_cluster(zone_a, "cluster_A1", {"A1_h1", "A1_h2"});
  const sg4::Link* llA1 = zone_a->create_link(std::string("") + "A1" + "plink", "1Gbps")->set_latency("100us");
  zone_a->add_route(clusterA1, nullptr, {sg4::LinkInRoute(llA1, sg4::LinkInRoute::Direction::UP)}, true);
  /* create cluster_A2 */
  simgrid::s4u::NetZone* clusterA2 = create_cluster(zone_a, "cluster_A2", {"A2_h1"});
  const sg4::Link* llA2 = zone_a->create_link(std::string("") + "A2" + "plink", "1Gbps")->set_latency("100us");
  zone_a->add_route(clusterA2, nullptr, {sg4::LinkInRoute(llA2, sg4::LinkInRoute::Direction::UP)}, true);
  /* seal zone_A */
  zone_a->seal();

  /* create zone_B */
  simgrid::s4u::NetZone* zone_b = sg4::create_star_zone("zone_B");
  zone_b->set_parent(root);
  zone_b->set_gateway(zone_b->create_router(std::string("zone_B") + "_router"));
  /* create cluster_B1 */
  simgrid::s4u::NetZone* clusterB1 = create_cluster(zone_b, "cluster_B1", {"B1_h1"});
  const sg4::Link* llB1 = zone_b->create_link(std::string("") + "B1" + "plink", "1Gbps")->set_latency("100us");
  zone_b->add_route(clusterB1, nullptr, {sg4::LinkInRoute(llB1, sg4::LinkInRoute::Direction::UP)}, true);
  /* seal zone_B */
  zone_b->seal();

  /* create zone_C */
  simgrid::s4u::NetZone* zone_c = sg4::create_star_zone("zone_C");
  zone_c->set_parent(root);
  zone_c->set_gateway(zone_c->create_router(std::string("zone_C") + "_router"));
  /* create host*_C1 */
  for (int i = 1; i <= 2; i++) {
    std::string hostname = std::string("C1_h") + std::to_string(i);
    sg4::Host* host      = zone_c->create_host(hostname, "1Gf");
    const sg4::Link* ll  = zone_c->create_link(hostname + "link", "1Gbps")->set_latency("100us");
    zone_c->add_route(host, nullptr, {sg4::LinkInRoute(ll, sg4::LinkInRoute::Direction::UP)}, true);
  }

  const sg4::Link* rootlink = root->create_link(std::string("rootlink"), "1Gbps")->set_latency("100us");
  root->add_route(zone_a, nullptr, {sg4::LinkInRoute(rootlink, sg4::LinkInRoute::Direction::UP)}, true);
  root->add_route(zone_b, nullptr, {sg4::LinkInRoute(rootlink, sg4::LinkInRoute::Direction::UP)}, true);
  root->add_route(zone_c, nullptr, {sg4::LinkInRoute(rootlink, sg4::LinkInRoute::Direction::UP)}, true);
  root->seal();
}