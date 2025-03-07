/* Copyright (c) 2014-2025. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "simgrid/s4u.hpp"

XBT_LOG_NEW_DEFAULT_CATEGORY(s4u_test, "Messages specific for this example");

static void computation_fun(simgrid::s4u::ExecPtr& exec)
{
  const char* pr_name   = simgrid::s4u::this_actor::get_cname();
  const char* host_name = simgrid::s4u::Host::current()->get_cname();
  double clock_sta      = simgrid::s4u::Engine::get_clock();

  XBT_INFO("%s:%s Exec 1 start %g", host_name, pr_name, clock_sta);
  exec = simgrid::s4u::this_actor::exec_async(1e9);
  exec->wait();
  XBT_INFO("%s:%s Exec 1 complete %g", host_name, pr_name, simgrid::s4u::Engine::get_clock() - clock_sta);

  exec = nullptr;

  simgrid::s4u::this_actor::sleep_for(1);

  clock_sta = simgrid::s4u::Engine::get_clock();
  XBT_INFO("%s:%s Exec 2 start %g", host_name, pr_name, clock_sta);
  exec = simgrid::s4u::this_actor::exec_async(1e10);
  exec->wait();
  XBT_INFO("%s:%s Exec 2 complete %g", host_name, pr_name, simgrid::s4u::Engine::get_clock() - clock_sta);
}

static void master_main()
{
  simgrid::s4u::Engine& e = *simgrid::s4u::this_actor::get_engine();
  auto* pm0 = simgrid::s4u::Host::by_name("Fafard");
  auto* vm0 = pm0->create_vm("VM0", 1);
  vm0->start();

  simgrid::s4u::ExecPtr exec;
  e.add_actor("compute", vm0, computation_fun, std::ref(exec));

  while (simgrid::s4u::Engine::get_clock() < 100) {
    if (exec)
      XBT_INFO("exec remaining duration: %g", exec->get_remaining());
    simgrid::s4u::this_actor::sleep_for(1);
  }

  simgrid::s4u::this_actor::sleep_for(10000);
  vm0->destroy();
}

int main(int argc, char* argv[])
{
  simgrid::s4u::Engine e(&argc, argv);

  e.load_platform(argv[1]);

  e.add_actor("master_", e.host_by_name("Fafard"), master_main);

  e.run();
  XBT_INFO("Bye (simulation time %g)", simgrid::s4u::Engine::get_clock());

  return 0;
}
