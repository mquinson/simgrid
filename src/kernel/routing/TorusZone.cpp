/* Copyright (c) 2014-2016. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/kernel/routing/TorusZone.hpp"
#include "src/kernel/routing/NetPoint.hpp"
#include "src/surf/network_interface.hpp"
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <string>
#include <vector>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_route_cluster_torus, surf_route_cluster, "Torus Routing part of surf");

inline void rankId_to_coords(int rankId, std::vector<unsigned int> dimensions, unsigned int (*coords)[4])
{
  unsigned int dim_size_product = 1;
  unsigned int i = 0;
  for (auto cur_dim_size: dimensions) {
    (*coords)[i] = (rankId / dim_size_product) % cur_dim_size;
    dim_size_product *= cur_dim_size;
    i++;
  }
}

namespace simgrid {
namespace kernel {
namespace routing {
TorusZone::TorusZone(NetZone* father, const char* name) : ClusterZone(father, name)
{
}

void TorusZone::create_links_for_node(sg_platf_cluster_cbarg_t cluster, int id, int rank, int position)
{
  /* Create all links that exist in the torus. Each rank creates @a dimensions-1 links */
  int dim_product = 1; // Needed to calculate the next neighbor_id

  for (unsigned int j = 0; j < dimensions_.size(); j++) {
    LinkCreationArgs link;
    int current_dimension = dimensions_.at(j); // which dimension are we currently in?
                                               // we need to iterate over all dimensions and create all links there
    // The other node the link connects
    int neighbor_rank_id = ((static_cast<int>(rank) / dim_product) % current_dimension == current_dimension - 1)
                               ? rank - (current_dimension - 1) * dim_product
                               : rank + dim_product;
    // name of neighbor is not right for non contiguous cluster radicals (as id != rank in this case)
    char* link_id  = bprintf("%s_link_from_%i_to_%i", cluster->id, id, neighbor_rank_id);
    link.id        = link_id;
    link.bandwidth = cluster->bw;
    link.latency   = cluster->lat;
    link.policy    = cluster->sharing_policy;
    sg_platf_new_link(&link);
    surf::LinkImpl* linkUp;
    surf::LinkImpl* linkDown;
    if (link.policy == SURF_LINK_FULLDUPLEX) {
      char* tmp_link = bprintf("%s_UP", link_id);
      linkUp         = surf::LinkImpl::byName(tmp_link);
      free(tmp_link);
      tmp_link = bprintf("%s_DOWN", link_id);
      linkDown = surf::LinkImpl::byName(tmp_link);
      free(tmp_link);
    } else {
      linkUp   = surf::LinkImpl::byName(link_id);
      linkDown = linkUp;
    }
    /*
     * Add the link to its appropriate position;
     * note that position rankId*(xbt_dynar_length(dimensions)+has_loopback?+has_limiter?)
     * holds the link "rankId->rankId"
     */
    privateLinks_.insert({position + j, {linkUp, linkDown}});
    dim_product *= current_dimension;
    xbt_free(link_id);
  }
  rank++;
}

void TorusZone::parse_specific_arguments(sg_platf_cluster_cbarg_t cluster)
{
  std::vector<std::string> dimensions;
  boost::split(dimensions, cluster->topo_parameters, boost::is_any_of(","));

  if (not dimensions.empty()) {
    /* We are in a torus cluster
     * Parse attribute dimensions="dim1,dim2,dim3,...,dimN" and safe it in a vector.
     * Additionally, we need to know how many ranks we have in total
     */
    for (auto group : dimensions) {
      dimensions_.push_back(surf_parse_get_int(group.c_str()));
    }

    linkCountPerNode_ = dimensions_.size();
  }
}

void TorusZone::getLocalRoute(NetPoint* src, NetPoint* dst, sg_platf_route_cbarg_t route, double* lat)
{

  XBT_VERB("torus getLocalRoute from '%s'[%d] to '%s'[%d]", src->name().c_str(), src->id(), dst->name().c_str(),
           dst->id());

  if (dst->isRouter() || src->isRouter())
    return;

  if (src->id() == dst->id() && hasLoopback_) {
    std::pair<surf::LinkImpl*, surf::LinkImpl*> info = privateLinks_.at(src->id() * linkCountPerNode_);

    route->link_list->push_back(info.first);
    if (lat)
      *lat += info.first->latency();
    return;
  }

  /*
   * Dimension based routing routes through each dimension consecutively
   * TODO Change to dynamic assignment
   */
  unsigned int dim_product = 1;
  unsigned int current_node = src->id();
  unsigned int next_node    = 0;
  /*
   * Arrays that hold the coordinates of the current node and
   * the target; comparing the values at the i-th position of
   * both arrays, we can easily assess whether we need to route
   * into this dimension or not.
   */
  unsigned int myCoords[4];
  rankId_to_coords(src->id(), dimensions_, &myCoords);
  unsigned int targetCoords[4];
  rankId_to_coords(dst->id(), dimensions_, &targetCoords);
  /*
   * linkOffset describes the offset where the link
   * we want to use is stored
   * (+1 is added because each node has a link from itself to itself,
   * which can only be the case if src->m_id == dst->m_id -- see above
   * for this special case)
   */
  int nodeOffset = (dimensions_.size() + 1) * src->id();

  int linkOffset  = nodeOffset;
  bool use_lnk_up = false; // Is this link of the form "cur -> next" or "next -> cur"?
  // false means: next -> cur
  while (current_node != dst->id()) {
    dim_product = 1; // First, we will route in x-dimension
    int j=0;
    for (auto cur_dim : dimensions_){
      // current_node/dim_product = position in current dimension
      if ((current_node / dim_product) % cur_dim != (dst->id() / dim_product) % cur_dim) {

        if ((targetCoords[j] > myCoords[j] &&
             targetCoords[j] <= myCoords[j] + cur_dim / 2) // Is the target node on the right, without the wrap-around?
            || (myCoords[j] > cur_dim / 2 &&
                (myCoords[j] + cur_dim / 2) % cur_dim >=
                    targetCoords[j])) { // Or do we need to use the wrap around to reach it?
          if ((current_node / dim_product) % cur_dim == cur_dim - 1)
            next_node = (current_node + dim_product - dim_product * cur_dim);
          else
            next_node = (current_node + dim_product);

          // HERE: We use *CURRENT* node for calculation (as opposed to next_node)
          nodeOffset = current_node * (linkCountPerNode_);
          linkOffset = nodeOffset + (hasLoopback_ ? 1 : 0) + (hasLimiter_ ? 1 : 0) + j;
          use_lnk_up = true;
          assert(linkOffset >= 0);
        } else { // Route to the left
          if ((current_node / dim_product) % cur_dim == 0)
            next_node = (current_node - dim_product + dim_product * cur_dim);
          else
            next_node = (current_node - dim_product);

          // HERE: We use *next* node for calculation (as opposed to current_node!)
          nodeOffset = next_node * (linkCountPerNode_);
          linkOffset = nodeOffset + j + (hasLoopback_ ? 1 : 0) + (hasLimiter_ ? 1 : 0);
          use_lnk_up = false;

          assert(linkOffset >= 0);
        }
        XBT_DEBUG("torus_get_route_and_latency - current_node: %i, next_node: %u, linkOffset is %i", current_node,
                  next_node, linkOffset);
        break;
      }

      j++;
      dim_product *= cur_dim;
    }

    std::pair<surf::LinkImpl*, surf::LinkImpl*> info;

    if (hasLimiter_) { // limiter for sender
      info = privateLinks_.at(nodeOffset + hasLoopback_);
      route->link_list->push_back(info.first);
    }

    info = privateLinks_.at(linkOffset);

    if (use_lnk_up == false) {
      route->link_list->push_back(info.second);
      if (lat)
        *lat += info.second->latency();
    } else {
      route->link_list->push_back(info.first);
      if (lat)
        *lat += info.first->latency();
    }
    current_node = next_node;
    next_node    = 0;
  }
}
}
}
} // namespace
