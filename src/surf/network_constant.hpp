/* Copyright (c) 2013-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef NETWORK_CONSTANT_HPP_
#define NETWORK_CONSTANT_HPP_

#include <xbt/base.h>

#include "network_interface.hpp"

namespace simgrid {
  namespace surf {

    /***********
     * Classes *
     ***********/

    class XBT_PRIVATE NetworkConstantModel;
    class XBT_PRIVATE NetworkConstantAction;

    /*********
     * Model *
     *********/
    class NetworkConstantModel : public NetworkModel {
    public:
      NetworkConstantModel()  : NetworkModel() { };
      ~NetworkConstantModel() override;

      Action *communicate(kernel::routing::NetCard *src, kernel::routing::NetCard *dst, double size, double rate) override;
      double next_occuring_event(double now) override;
      bool next_occuring_event_isIdempotent() override {return true;}
      void updateActionsState(double now, double delta) override;

      Link* createLink(const char *name, double bw, double lat, e_surf_link_sharing_policy_t policy, xbt_dict_t properties) override;
    };

    /**********
     * Action *
     **********/
    class NetworkConstantAction : public NetworkAction {
    public:
      NetworkConstantAction(NetworkConstantModel *model_, double size, double latency);
      ~NetworkConstantAction();
      double initialLatency_;
    };

  }
}

#endif /* NETWORK_CONSTANT_HPP_ */
