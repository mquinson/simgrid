/* A few basic tests for the surf library                                   */

/* Copyright (c) 2004-2015. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "surf/surf.h"
#include "simgrid/s4u/host.hpp"
#include "simgrid/sg_config.h"
#include "src/surf/cpu_interface.hpp"
#include "src/surf/network_interface.hpp"
#include "src/surf/surf_interface.hpp"
#include <stdio.h>

#include "xbt/log.h"
XBT_LOG_NEW_DEFAULT_CATEGORY(surf_test, "Messages specific for surf example");

static const char *string_action(simgrid::surf::Action::State state)
{
  switch (state) {
    case simgrid::surf::Action::State::ready:
      return "SURF_ACTION_READY";
    case simgrid::surf::Action::State::running:
      return "SURF_ACTION_RUNNING";
    case simgrid::surf::Action::State::failed:
      return "SURF_ACTION_FAILED";
    case simgrid::surf::Action::State::done:
      return "SURF_ACTION_DONE";
    case simgrid::surf::Action::State::not_in_the_system:
      return "SURF_ACTION_NOT_IN_THE_SYSTEM";
    default:
      return "INVALID STATE";
  }
}

int main(int argc, char **argv)
{
  double now = -1.0;
  surf_init(&argc, argv);       /* Initialize some common structures */
  xbt_cfg_set_parse("cpu/model:Cas01");
  xbt_cfg_set_parse("network/model:CM02");

  xbt_assert(argc > 1, "Usage: %s platform.xml\n", argv[0]);
  parse_platform_file(argv[1]);

  XBT_DEBUG("CPU model: %p", surf_cpu_model_pm);
  XBT_DEBUG("Network model: %p", surf_network_model);
  simgrid::s4u::Host* hostA = sg_host_by_name("Cpu A");
  simgrid::s4u::Host* hostB = sg_host_by_name("Cpu B");

  /* Let's do something on it */
  simgrid::surf::Action *actionA = hostA->pimpl_cpu->execution_start(1000.0);
  simgrid::surf::Action *actionB = hostB->pimpl_cpu->execution_start(1000.0);
  simgrid::surf::Action *actionC = surf_host_sleep(hostB, 7.32);

  simgrid::surf::Action::State stateActionA = actionA->getState();
  simgrid::surf::Action::State stateActionB = actionB->getState();
  simgrid::surf::Action::State stateActionC = actionC->getState();

  /* And just look at the state of these tasks */
  XBT_INFO("actionA state: %s", string_action(stateActionA));
  XBT_INFO("actionB state: %s", string_action(stateActionB));
  XBT_INFO("actionC state: %s", string_action(stateActionC));


  /* Let's do something on it */
  surf_network_model->communicate(hostA, hostB, 150.0, -1.0);

  surf_solve(-1.0);
  do {
    simgrid::surf::ActionList *action_list = nullptr;
    now = surf_get_clock();
    XBT_INFO("Next Event : %g", now);
    XBT_DEBUG("\t CPU actions");

    action_list = surf_cpu_model_pm->getFailedActionSet();
    for(simgrid::surf::ActionList::iterator it(action_list->begin()), itNext = it, itend(action_list->end()) ;
        it != itend ; it=itNext) {
      ++itNext;
      simgrid::surf::Action *action = static_cast<simgrid::surf::CpuAction*>(&*it);
       XBT_INFO("   CPU Failed action");
       XBT_DEBUG("\t * Failed : %p", action);
       action->unref();
    }

    action_list = surf_cpu_model_pm->getDoneActionSet();
    for(simgrid::surf::ActionList::iterator it(action_list->begin()), itNext = it, itend(action_list->end()) ;
        it != itend ; it=itNext) {
      ++itNext;
      simgrid::surf::Action *action = static_cast<simgrid::surf::CpuAction*>(&*it);
      XBT_INFO("   CPU Done action");
      XBT_DEBUG("\t * Done : %p", action);
      action->unref();
    }

    action_list = surf_network_model->getFailedActionSet();
    for(simgrid::surf::ActionList::iterator it(action_list->begin()), itNext = it, itend(action_list->end()) ;
        it != itend ; it=itNext) {
      ++itNext;
      simgrid::surf::Action *action = static_cast<simgrid::surf::NetworkAction*>(&*it);
       XBT_INFO("   Network Failed action");
       XBT_DEBUG("\t * Failed : %p", action);
       action->unref();
    }

    action_list = surf_network_model->getDoneActionSet();
    for(simgrid::surf::ActionList::iterator it(action_list->begin()), itNext = it, itend(action_list->end()) ;
        it != itend ; it=itNext) {
      ++itNext;
      simgrid::surf::Action *action = static_cast<simgrid::surf::NetworkAction*>(&*it);
      XBT_INFO("   Network Done action");
      XBT_DEBUG("\t * Done : %p", action);
      action->unref();
    }

  } while ((surf_network_model->getRunningActionSet()->size() ||
           surf_cpu_model_pm->getRunningActionSet()->size()) && surf_solve(-1.0) >= 0.0);

  XBT_DEBUG("Simulation Terminated");

  surf_exit();
  return 0;
}
