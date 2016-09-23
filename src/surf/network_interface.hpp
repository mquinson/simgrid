/* Copyright (c) 2004-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SURF_NETWORK_INTERFACE_HPP_
#define SURF_NETWORK_INTERFACE_HPP_

#include <xbt/base.h>

#include <boost/unordered_map.hpp>

#include "xbt/fifo.h"
#include "xbt/dict.h"
#include "surf_interface.hpp"
#include "surf_routing.hpp"
#include "src/surf/PropertyHolder.hpp"

#include "simgrid/link.h"

/***********
 * Classes *
 ***********/

namespace simgrid {
  namespace surf {

    class NetworkAction;

    /** @brief Callback signal fired when the state of a NetworkAction changes
     *  Signature: `void(NetworkAction *action, simgrid::surf::Action::State old, simgrid::surf::Action::State current)` */
    XBT_PUBLIC_DATA(simgrid::xbt::signal<void(simgrid::surf::NetworkAction*, simgrid::surf::Action::State, simgrid::surf::Action::State)>) networkActionStateChangedCallbacks;

  }
}
/*********
 * Model *
 *********/

namespace simgrid {
  namespace surf {

    /** @ingroup SURF_network_interface
     * @brief SURF network model interface class
     * @details A model is an object which handles the interactions between its Resources and its Actions
     */
    class NetworkModel : public Model {
    public:
      /** @brief Constructor */
      NetworkModel() : Model() { }

      /** @brief Destructor */
      ~NetworkModel() override;

      /**
       * @brief Create a Link
       *
       * @param name The name of the Link
       * @param bandwidth The initial bandwidth of the Link in bytes per second
       * @param latency The initial latency of the Link in seconds
       * @param policy The sharing policy of the Link
       * @param props Dictionary of properties associated to this Link
       */
      virtual Link* createLink(const char *name, double bandwidth, double latency,
          e_surf_link_sharing_policy_t policy, xbt_dict_t properties)=0;

      /**
       * @brief Create a communication between two hosts.
       * @details It makes calls to the routing part, and execute the communication
       *          between the two end points.
       *
       * @param src The source of the communication
       * @param dst The destination of the communication
       * @param size The size of the communication in bytes
       * @param rate Allows to limit the transfer rate. Negative value means
       * unlimited.
       * @return The action representing the communication
       */
      virtual Action *communicate(kernel::routing::NetCard *src, kernel::routing::NetCard *dst, double size, double rate)=0;

      /** @brief Function pointer to the function to use to solve the lmm_system_t
       *
       * @param system The lmm_system_t to solve
       */
      void (*f_networkSolve)(lmm_system_t) = lmm_solve;

      /**
       * @brief Get the right multiplicative factor for the latency.
       * @details Depending on the model, the effective latency when sending
       * a message might be different from the theoretical latency of the link,
       * in function of the message size. In order to account for this, this
       * function gets this factor.
       *
       * @param size The size of the message.
       * @return The latency factor.
       */
      virtual double latencyFactor(double size);

      /**
       * @brief Get the right multiplicative factor for the bandwidth.
       * @details Depending on the model, the effective bandwidth when sending
       * a message might be different from the theoretical bandwidth of the link,
       * in function of the message size. In order to account for this, this
       * function gets this factor.
       *
       * @param size The size of the message.
       * @return The bandwidth factor.
       */
      virtual double bandwidthFactor(double size);

      /**
       * @brief Get definitive bandwidth.
       * @details It gives the minimum bandwidth between the one that would
       * occur if no limitation was enforced, and the one arbitrary limited.
       * @param rate The desired maximum bandwidth.
       * @param bound The bandwidth with only the network taken into account.
       * @param size The size of the message.
       * @return The new bandwidth.
       */
      virtual double bandwidthConstraint(double rate, double bound, double size);
      double next_occuring_event_full(double now) override;
    };

    /************
     * Resource *
     ************/
    /** @ingroup SURF_network_interface
     * @brief SURF network link interface class
     * @details A Link represents the link between two [hosts](\ref simgrid::surf::HostImpl)
     */
    class Link :
        public simgrid::surf::Resource,
        public simgrid::surf::PropertyHolder {
        public:

      /** @brief Constructor of non-LMM links */
      Link(simgrid::surf::NetworkModel *model, const char *name, xbt_dict_t props);
      /** @brief Constructor of LMM links */
      Link(simgrid::surf::NetworkModel *model, const char *name, xbt_dict_t props, lmm_constraint_t constraint);

      /* Link destruction logic */
      /**************************/
        protected:
      ~Link() override;
        public:
      void destroy(); // Must be called instead of the destructor
        private:
      bool currentlyDestroying_ = false;

        public:
      /** @brief Callback signal fired when a new Link is created.
       *  Signature: void(Link*) */
      static simgrid::xbt::signal<void(surf::Link*)> onCreation;

      /** @brief Callback signal fired when a Link is destroyed.
       *  Signature: void(Link*) */
      static simgrid::xbt::signal<void(surf::Link*)> onDestruction;

      /** @brief Callback signal fired when the state of a Link changes (when it is turned on or off)
       *  Signature: `void(Link*)` */
      static simgrid::xbt::signal<void(surf::Link*)> onStateChange;

      /** @brief Callback signal fired when a communication starts
       *  Signature: `void(NetworkAction *action, RoutingEdge *src, RoutingEdge *dst)` */
      static simgrid::xbt::signal<void(surf::NetworkAction*, kernel::routing::NetCard *src, kernel::routing::NetCard *dst)> onCommunicate;



      /** @brief Get the bandwidth in bytes per second of current Link */
      virtual double getBandwidth();

      /** @brief Update the bandwidth in bytes per second of current Link */
      virtual void updateBandwidth(double value)=0;

      /** @brief Get the latency in seconds of current Link */
      virtual double getLatency();

      /** @brief Update the latency in seconds of current Link */
      virtual void updateLatency(double value)=0;

      /** @brief The sharing policy is a @{link e_surf_link_sharing_policy_t::EType} (0: FATPIPE, 1: SHARED, 2: FULLDUPLEX) */
      virtual int sharingPolicy();

      /** @brief Check if the Link is used */
      bool isUsed() override;

      void turnOn() override;
      void turnOff() override;

      virtual void setStateTrace(tmgr_trace_t trace); /*< setup the trace file with states events (ON or OFF). Trace must contain boolean values. */
      virtual void setBandwidthTrace(tmgr_trace_t trace); /*< setup the trace file with bandwidth events (peak speed changes due to external load). Trace must contain percentages (value between 0 and 1). */
      virtual void setLatencyTrace(tmgr_trace_t trace); /*< setup the trace file with latency events (peak latency changes due to external load). Trace must contain absolute values */

      tmgr_trace_iterator_t m_stateEvent = nullptr;
      s_surf_metric_t m_latency = {1.0,0,nullptr};
      s_surf_metric_t m_bandwidth = {1.0,0,nullptr};

      /* User data */
        public:
      void *getData()        { return userData;}
      void  setData(void *d) { userData=d;}
        private:
      void *userData = nullptr;

      /* List of all links */
        private:
      static boost::unordered_map<std::string, Link *> *links;
        public:
      static Link *byName(const char* name);
      static int linksCount();
      static Link **linksList();
      static void linksExit();
    };

    /**********
     * Action *
     **********/
    /** @ingroup SURF_network_interface
     * @brief SURF network action interface class
     * @details A NetworkAction represents a communication between two [hosts](\ref HostImpl)
     */
    class NetworkAction : public simgrid::surf::Action {
    public:
      /** @brief Constructor
       *
       * @param model The NetworkModel associated to this NetworkAction
       * @param cost The cost of this  NetworkAction in [TODO]
       * @param failed [description]
       */
      NetworkAction(simgrid::surf::Model *model, double cost, bool failed)
    : simgrid::surf::Action(model, cost, failed) {}

      /**
       * @brief NetworkAction constructor
       *
       * @param model The NetworkModel associated to this NetworkAction
       * @param cost The cost of this  NetworkAction in [TODO]
       * @param failed [description]
       * @param var The lmm variable associated to this Action if it is part of a
       * LMM component
       */
      NetworkAction(simgrid::surf::Model *model, double cost, bool failed, lmm_variable_t var)
      : simgrid::surf::Action(model, cost, failed, var) {};

      void setState(simgrid::surf::Action::State state) override;

      double latency_;
      double latCurrent_;
      double weight_;
      double rate_;
      const char* senderLinkName_;
      double senderSize_;
      xbt_fifo_item_t senderFifoItem_;
    };
  }
}

#endif /* SURF_NETWORK_INTERFACE_HPP_ */


