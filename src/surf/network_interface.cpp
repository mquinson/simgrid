/* Copyright (c) 2013-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <algorithm>

#include "network_interface.hpp"
#include "simgrid/sg_config.h"

#ifndef NETWORK_INTERFACE_CPP_
#define NETWORK_INTERFACE_CPP_

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_network, surf, "Logging specific to the SURF network module");

/*********
 * C API *
 *********/

extern "C" {

  const char* sg_link_name(Link *link) {
    return link->getName();
  }
  Link * sg_link_by_name(const char* name) {
    return Link::byName(name);
  }

  int sg_link_is_shared(Link *link){
    return link->sharingPolicy();
  }
  double sg_link_bandwidth(Link *link){
    return link->bandwidth();
  }
  double sg_link_latency(Link *link){
    return link->latency();
  }
  void* sg_link_data(Link *link) {
    return link->getData();
  }
  void sg_link_data_set(Link *link,void *data) {
    link->setData(data);
  }
  int sg_link_count() {
    return Link::linksCount();
  }
  Link** sg_link_list() {
    return Link::linksList();
  }
  void sg_link_exit() {
    Link::linksExit();
  }

}

/*****************
 * List of links *
 *****************/

namespace simgrid {
  namespace surf {

    std::unordered_map<std::string,Link *> *Link::links = new std::unordered_map<std::string,Link *>();
    Link *Link::byName(const char* name) {
      if (links->find(name) == links->end())
        return nullptr;
      return  links->at(name);
    }
    /** @brief Returns the amount of links in the platform */
    int Link::linksCount() {
      return links->size();
    }
    /** @brief Returns a list of all existing links */
    Link **Link::linksList() {
      Link **res = xbt_new(Link*, (int)links->size());
      int i=0;
      for (auto kv : *links) {
        res[i++] = kv.second;
      }
      return res;
    }
    /** @brief destructor of the static data */
    void Link::linksExit() {
      for (auto kv : *links)
        (kv.second)->destroy();
      delete links;
    }

    /*************
     * Callbacks *
     *************/

    simgrid::xbt::signal<void(Link*)> Link::onCreation;
    simgrid::xbt::signal<void(Link*)> Link::onDestruction;
    simgrid::xbt::signal<void(Link*)> Link::onStateChange;

    simgrid::xbt::signal<void(NetworkAction*, Action::State, Action::State)> networkActionStateChangedCallbacks;
    simgrid::xbt::signal<void(NetworkAction*, s4u::Host* src, s4u::Host* dst)> Link::onCommunicate;
  }
}

/*********
 * Model *
 *********/

simgrid::surf::NetworkModel *surf_network_model = nullptr;

namespace simgrid {
  namespace surf {

    NetworkModel::~NetworkModel()
    {
      lmm_system_free(maxminSystem_);
      xbt_heap_free(actionHeap_);
      delete modifiedSet_;
    }

    double NetworkModel::latencyFactor(double /*size*/) {
      return sg_latency_factor;
    }

    double NetworkModel::bandwidthFactor(double /*size*/) {
      return sg_bandwidth_factor;
    }

    double NetworkModel::bandwidthConstraint(double rate, double /*bound*/, double /*size*/) {
      return rate;
    }

    double NetworkModel::nextOccuringEventFull(double now)
    {
      double minRes = Model::nextOccuringEventFull(now);

      for(auto it(getRunningActionSet()->begin()), itend(getRunningActionSet()->end()); it != itend ; it++) {
        NetworkAction *action = static_cast<NetworkAction*>(&*it);
        if (action->latency_ > 0)
          minRes = (minRes < 0) ? action->latency_ : std::min(minRes, action->latency_);
      }

      XBT_DEBUG("Min of share resources %f", minRes);

      return minRes;
    }

    /************
     * Resource *
     ************/

    Link::Link(simgrid::surf::NetworkModel* model, const char* name, lmm_constraint_t constraint)
        : Resource(model, name, constraint)
    {
      if (strcmp(name,"__loopback__"))
        xbt_assert(!Link::byName(name), "Link '%s' declared several times in the platform.", name);

      latency_.scale   = 1;
      bandwidth_.scale = 1;

      links->insert({name, this});
      XBT_DEBUG("Create link '%s'",name);

    }

    /** @brief use destroy() instead of this destructor */
    Link::~Link() {
      xbt_assert(currentlyDestroying_, "Don't delete Links directly. Call destroy() instead.");
    }
    /** @brief Fire the required callbacks and destroy the object
     *
     * Don't delete directly a Link, call l->destroy() instead.
     */
    void Link::destroy()
    {
      if (!currentlyDestroying_) {
        currentlyDestroying_ = true;
        onDestruction(this);
        delete this;
      }
    }

    bool Link::isUsed()
    {
      return lmm_constraint_used(getModel()->getMaxminSystem(), getConstraint());
    }

    double Link::latency()
    {
      return latency_.peak * latency_.scale;
    }

    double Link::bandwidth()
    {
      return bandwidth_.peak * bandwidth_.scale;
    }

    int Link::sharingPolicy()
    {
      return lmm_constraint_sharing_policy(getConstraint());
    }

    void Link::turnOn(){
      if (isOff()) {
        Resource::turnOn();
        onStateChange(this);
      }
    }
    void Link::turnOff(){
      if (isOn()) {
        Resource::turnOff();
        onStateChange(this);
      }
    }
    void Link::setStateTrace(tmgr_trace_t trace) {
      xbt_assert(stateEvent_ == nullptr, "Cannot set a second state trace to Link %s", getName());
      stateEvent_ = future_evt_set->add_trace(trace, 0.0, this);
    }
    void Link::setBandwidthTrace(tmgr_trace_t trace)
    {
      xbt_assert(bandwidth_.event == nullptr, "Cannot set a second bandwidth trace to Link %s", getName());
      bandwidth_.event = future_evt_set->add_trace(trace, 0.0, this);
    }
    void Link::setLatencyTrace(tmgr_trace_t trace)
    {
      xbt_assert(latency_.event == nullptr, "Cannot set a second latency trace to Link %s", getName());
      latency_.event = future_evt_set->add_trace(trace, 0.0, this);
    }


    /**********
     * Action *
     **********/

    void NetworkAction::setState(Action::State state){
      Action::State old = getState();
      Action::setState(state);
      networkActionStateChangedCallbacks(this, old, state);
    }

  }
}

#endif /* NETWORK_INTERFACE_CPP_ */
