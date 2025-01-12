/* Copyright (c) 2018-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */
#include <pybind11/pybind11.h> // Must come before our own stuff

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include "simgrid/kernel/ProfileBuilder.hpp"
#include "simgrid/kernel/routing/NetPoint.hpp"
#include "simgrid/modelchecker.h"
#include "simgrid/plugins/load.h"
#include <simgrid/Exception.hpp>
#include <simgrid/s4u/ActivitySet.hpp>
#include <simgrid/s4u/Actor.hpp>
#include <simgrid/s4u/Barrier.hpp>
#include <simgrid/s4u/Comm.hpp>
#include <simgrid/s4u/Disk.hpp>
#include <simgrid/s4u/Engine.hpp>
#include <simgrid/s4u/Exec.hpp>
#include <simgrid/s4u/Host.hpp>
#include <simgrid/s4u/Io.hpp>
#include <simgrid/s4u/Link.hpp>
#include <simgrid/s4u/Mailbox.hpp>
#include <simgrid/s4u/Mutex.hpp>
#include <simgrid/s4u/NetZone.hpp>
#include <simgrid/s4u/Semaphore.hpp>
#include <simgrid/s4u/Task.hpp>
#include <simgrid/version.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;
using simgrid::s4u::Activity;
using simgrid::s4u::ActivityPtr;
using simgrid::s4u::ActivitySet;
using simgrid::s4u::ActivitySetPtr;
using simgrid::s4u::Actor;
using simgrid::s4u::ActorPtr;
using simgrid::s4u::Barrier;
using simgrid::s4u::BarrierPtr;
using simgrid::s4u::Comm;
using simgrid::s4u::CommPtr;
using simgrid::s4u::CommTask;
using simgrid::s4u::CommTaskPtr;
using simgrid::s4u::Disk;
using simgrid::s4u::Engine;
using simgrid::s4u::ExecTask;
using simgrid::s4u::ExecTaskPtr;
using simgrid::s4u::Host;
using simgrid::s4u::Io;
using simgrid::s4u::IoTask;
using simgrid::s4u::IoTaskPtr;
using simgrid::s4u::Link;
using simgrid::s4u::Mailbox;
using simgrid::s4u::Mutex;
using simgrid::s4u::MutexPtr;
using simgrid::s4u::Semaphore;
using simgrid::s4u::SemaphorePtr;
using simgrid::s4u::Task;
using simgrid::s4u::TaskPtr;

XBT_LOG_NEW_DEFAULT_CATEGORY(python, "python");

#ifdef SIMGRID_PYTHON_SMPI
void SMPI_bindings(py::module& m);
#endif
namespace {

std::string get_simgrid_version()
{
  int major;
  int minor;
  int patch;
  sg_version_get(&major, &minor, &patch);
  return simgrid::xbt::string_printf("%i.%i.%i", major, minor, patch);
}
} // namespace

PYBIND11_DECLARE_HOLDER_TYPE(T, boost::intrusive_ptr<T>)

PYBIND11_MODULE(simgrid, m)
{
  m.doc() = "SimGrid userspace API";

  m.attr("simgrid_version") = get_simgrid_version();

  m.def("MC_assert", &MC_assert, "Assertion for the model-checker: Defines a safety property to verify");
  m.def("MC_random", &MC_random,
        "Explore every branches where that function returns a value between min and max (inclusive)");

  // Swapped contexts are broken, starting from pybind11 v2.8.0.  Use thread contexts by default.
  simgrid::s4u::Engine::set_config("contexts/factory:thread");

  // Internal exception used to kill actors and sweep the RAII chimney (free objects living on the stack)
  static py::object pyForcefulKillEx(py::register_exception<simgrid::ForcefulKillException>(m, "ActorKilled"));

  py::register_exception<simgrid::NetworkFailureException>(m, "NetworkFailureException");
  py::register_exception<simgrid::TimeoutException>(m, "TimeoutException");
  py::register_exception<simgrid::HostFailureException>(m, "HostFailureException");
  py::register_exception<simgrid::StorageFailureException>(m, "StorageFailureException");
  py::register_exception<simgrid::VmFailureException>(m, "VmFailureException");
  py::register_exception<simgrid::CancelException>(m, "CancelException");
  py::register_exception<simgrid::AssertionError>(m, "AssertionError");

  /* this_actor namespace */
  m.def_submodule("this_actor", "Bindings of the s4u::this_actor namespace. See the C++ documentation for details.")
      .def(
          "debug", [](const char* s) { XBT_DEBUG("%s", s); }, "Display a logging message of 'debug' priority.")
      .def(
          "info", [](const char* s) { XBT_INFO("%s", s); }, "Display a logging message of 'info' priority.")
      .def(
          "warning", [](const char* s) { XBT_WARN("%s", s); }, "Display a logging message of 'warning' priority.")
      .def(
          "error", [](const char* s) { XBT_ERROR("%s", s); }, "Display a logging message of 'error' priority.")
      .def("execute", py::overload_cast<double, double>(&simgrid::s4u::this_actor::execute),
           py::call_guard<py::gil_scoped_release>(),
           "Block the current actor, computing the given amount of flops at the given priority.", py::arg("flops"),
           py::arg("priority") = 1)
      .def("exec_init", py::overload_cast<double>(&simgrid::s4u::this_actor::exec_init),
           py::call_guard<py::gil_scoped_release>())
      .def("exec_async", py::overload_cast<double>(&simgrid::s4u::this_actor::exec_async),
           py::call_guard<py::gil_scoped_release>())
      .def("parallel_execute", &simgrid::s4u::this_actor::parallel_execute, py::call_guard<py::gil_scoped_release>(),
           "Run a parallel task (requires the 'ptask_L07' model)")
      .def("exec_init",
           py::overload_cast<const std::vector<simgrid::s4u::Host*>&, const std::vector<double>&,
                             const std::vector<double>&>(&simgrid::s4u::this_actor::exec_init),
           py::call_guard<py::gil_scoped_release>(), "Initiate a parallel task (requires the 'ptask_L07' model)")
      .def("get_host", &simgrid::s4u::this_actor::get_host, "Retrieves host on which the current actor is located")
      .def("set_host", &simgrid::s4u::this_actor::set_host, py::call_guard<py::gil_scoped_release>(),
           "Moves the current actor to another host.", py::arg("dest"))
      .def("sleep_for", static_cast<void (*)(double)>(&simgrid::s4u::this_actor::sleep_for),
           py::call_guard<py::gil_scoped_release>(), "Block the actor sleeping for that amount of seconds.",
           py::arg("duration"))
      .def("sleep_until", static_cast<void (*)(double)>(&simgrid::s4u::this_actor::sleep_until),
           py::call_guard<py::gil_scoped_release>(), "Block the actor sleeping until the specified timestamp.",
           py::arg("duration"))
      .def("suspend", &simgrid::s4u::this_actor::suspend, py::call_guard<py::gil_scoped_release>(),
           "Suspend the current actor, that is blocked until resume()ed by another actor.")
      .def("yield_", &simgrid::s4u::this_actor::yield, py::call_guard<py::gil_scoped_release>(), "Yield the actor")
      .def("exit", &simgrid::s4u::this_actor::exit, py::call_guard<py::gil_scoped_release>(), "kill the current actor")
      .def(
          "on_exit",
          [](py::object fun) {
            fun.inc_ref(); // keep alive after return
            const py::gil_scoped_release gil_release;
            simgrid::s4u::this_actor::on_exit([fun_p = fun.ptr()](bool failed) {
              const py::gil_scoped_acquire py_context; // need a new context for callback
              try {
                const auto fun = py::reinterpret_borrow<py::function>(fun_p);
                fun(failed);
              } catch (const py::error_already_set& e) {
                xbt_die("Error while executing the on_exit lambda: %s", e.what());
              }
            });
          },
          "Define a lambda to be called when the actor ends. It takes a bool parameter indicating whether the actor "
          "was killed. If False, the actor finished peacefully.")
      .def("get_pid", &simgrid::s4u::this_actor::get_pid, "Retrieves PID of the current actor")
      .def("get_ppid", &simgrid::s4u::this_actor::get_ppid,
           "Retrieves PPID of the current actor (i.e., the PID of its parent).")
      .def("get_name", &simgrid::s4u::this_actor::get_cname, "Retrieves the name of this actor.");

  /* Class Engine */
  py::class_<Engine>(m, "Engine", "Simulation Engine")
      .def(py::init([](std::vector<std::string> args) {
             auto argc = static_cast<int>(args.size());
             std::vector<char*> argv(args.size() + 1); // argv[argc] is nullptr
             std::transform(begin(args), end(args), begin(argv), [](std::string& s) { return &s.front(); });
             // Currently this can be dangling, we should wrap this somehow.
             return new simgrid::s4u::Engine(&argc, argv.data());
           }),
           "The constructor should take the parameters from the command line, as is ")
      .def_property_readonly_static(
          "clock", [](py::object /* self */) { return Engine::get_clock(); },
          "The simulation time, ie the amount of simulated seconds since the simulation start.")
      .def_property_readonly_static(
          "instance", [](py::object /* self */) { return Engine::get_instance(); }, "Retrieve the simulation engine")
      .def("host_by_name", &Engine::host_by_name_or_null,
           "Retrieve a host by its name, or None if it does not exist in the platform.")
      .def_property_readonly("all_hosts", &Engine::get_all_hosts, "Returns the list of all hosts found in the platform")
      .def_property_readonly("all_links", &Engine::get_all_links, "Returns the list of all links found in the platform")
      .def_property_readonly("all_netpoints", &Engine::get_all_netpoints, "Returns the list of all netpoints found in the platform")
      .def_property_readonly("all_actors", &Engine::get_all_actors, "Returns the list of all actors found in the platform")
      .def_property_readonly("netzone_root", &Engine::get_netzone_root,
                             "Retrieve the root netzone, containing all others.")
      .def("netpoint_by_name", &Engine::netpoint_by_name_or_null)
      .def("netzone_by_name", &Engine::netzone_by_name_or_null)
      .def("set_config", py::overload_cast<const std::string&>(&Engine::set_config),
           "Change one of SimGrid's configurations")
      .def("load_platform", &Engine::load_platform, "Load a platform file describing the environment")
      .def("load_deployment", &Engine::load_deployment, "Load a deployment file and launch the actors that it contains")
      .def("mailbox_by_name_or_create", &Engine::mailbox_by_name_or_create, py::call_guard<py::gil_scoped_release>(),
           py::arg("name"), "Find a mailbox from its name or create one if it does not exist")
      .def("run", &Engine::run, py::call_guard<py::gil_scoped_release>(), "Run the simulation until its end")
      .def("run_until", py::overload_cast<double>(&Engine::run_until, py::const_),
           py::call_guard<py::gil_scoped_release>(), "Run the simulation until the given date",
           py::arg("max_date") = -1)
      .def(
          "register_actor",
          [](Engine* e, const std::string& name, py::object fun_or_class) {
            fun_or_class.inc_ref(); // keep alive after return
            const py::gil_scoped_release gil_release;
            e->register_actor(name, [fun_or_class_p = fun_or_class.ptr()](std::vector<std::string> args) {
              const py::gil_scoped_acquire py_context;
              try {
                /* Convert the std::vector into a py::tuple */
                py::tuple params(args.size() - 1);
                for (size_t i = 1; i < args.size(); i++)
                  params[i - 1] = py::cast(args[i]);

                const auto fun_or_class = py::reinterpret_borrow<py::object>(fun_or_class_p);
                py::object res          = fun_or_class(*params);
                /* If I was passed a class, I just built an instance, so I need to call it now */
                if (py::isinstance<py::function>(res))
                  res();
              } catch (const py::error_already_set& ex) {
                if (ex.matches(pyForcefulKillEx)) {
                  XBT_VERB("Actor killed");
                  simgrid::ForcefulKillException::do_throw(); // Forward that ForcefulKill exception
                }
                throw;
              }
            });
          },
          "Registers the main function of an actor")
      .def("set_log_control", [](Engine*, const std::string& settings) { xbt_log_control_set(settings.c_str()); });

  /* Class Netzone */
  py::class_<simgrid::s4u::NetZone, std::unique_ptr<simgrid::s4u::NetZone, py::nodelete>> netzone(
      m, "NetZone", "Networking Zones. See the C++ documentation for details.");
  netzone.def_static("create_full_zone", &simgrid::s4u::create_full_zone, "Creates a zone of type FullZone")
      .def_static("create_torus_zone", &simgrid::s4u::create_torus_zone, "Creates a cluster of type Torus")
      .def_static("create_fatTree_zone", &simgrid::s4u::create_fatTree_zone, "Creates a cluster of type Fat-Tree")
      .def_static("create_dragonfly_zone", &simgrid::s4u::create_dragonfly_zone, "Creates a cluster of type Dragonfly")
      .def_static("create_star_zone", &simgrid::s4u::create_star_zone, "Creates a zone of type Star")
      .def_static("create_floyd_zone", &simgrid::s4u::create_floyd_zone, "Creates a zone of type Floyd")
      .def_static("create_dijkstra_zone", &simgrid::s4u::create_floyd_zone, "Creates a zone of type Dijkstra")
      .def_static("create_vivaldi_zone", &simgrid::s4u::create_vivaldi_zone, "Creates a zone of type Vivaldi")
      .def_static("create_empty_zone", &simgrid::s4u::create_empty_zone, "Creates a zone of type Empty")
      .def_static("create_wifi_zone", &simgrid::s4u::create_wifi_zone, "Creates a zone of type Wi-Fi")
      .def("add_component", &simgrid::s4u::NetZone::add_component, "Add NetPoint component to NetZone")
      .def("add_route",
           py::overload_cast<const simgrid::s4u::Host*, const simgrid::s4u::Host*,
                             const std::vector<simgrid::s4u::LinkInRoute>&, bool>(&simgrid::s4u::NetZone::add_route),
           "Add a route between 2 hosts")
      .def("add_route",
           py::overload_cast<const simgrid::s4u::Host*, const simgrid::s4u::Host*,
                             const std::vector<const simgrid::s4u::Link*>&>(&simgrid::s4u::NetZone::add_route),
           "Add a route between 2 hosts")
      .def("add_route",
           py::overload_cast<const simgrid::s4u::NetZone*, const simgrid::s4u::NetZone*,
                             const std::vector<simgrid::s4u::LinkInRoute>&, bool>(&simgrid::s4u::NetZone::add_route),
           "Add a route between 2 netzones. The gateway of each zone gets used.")
      .def("add_route",
           py::overload_cast<const simgrid::s4u::NetZone*, const simgrid::s4u::NetZone*,
                             const std::vector<const simgrid::s4u::Link*>&>(&simgrid::s4u::NetZone::add_route),
           "Add a route between 2 netzones. The gateway of each zone gets used.")
      .def("add_bypass_route", &simgrid::s4u::NetZone::add_bypass_route, "Add bypass route to NetZone.")
      .def("create_host", py::overload_cast<const std::string&, double>(&simgrid::s4u::NetZone::create_host),
           "Creates a host")
      .def("create_host",
           py::overload_cast<const std::string&, const std::string&>(&simgrid::s4u::NetZone::create_host),
           "Creates a host")
      .def("create_host",
           py::overload_cast<const std::string&, const std::vector<double>&>(&simgrid::s4u::NetZone::create_host),
           "Creates a host")
      .def("create_host",
           py::overload_cast<const std::string&, const std::vector<std::string>&>(&simgrid::s4u::NetZone::create_host),
           "Creates a host")
      .def("create_link", py::overload_cast<const std::string&, double>(&simgrid::s4u::NetZone::create_link),
           "Creates a network link")
      .def("create_link",
           py::overload_cast<const std::string&, const std::string&>(&simgrid::s4u::NetZone::create_link),
           "Creates a network link")
      .def("create_link",
           py::overload_cast<const std::string&, const std::vector<double>&>(&simgrid::s4u::NetZone::create_link),
           "Creates a network link")
      .def("create_link",
           py::overload_cast<const std::string&, const std::vector<std::string>&>(&simgrid::s4u::NetZone::create_link),
           "Creates a network link")
      .def("create_split_duplex_link",
           py::overload_cast<const std::string&, double>(&simgrid::s4u::NetZone::create_split_duplex_link),
           "Creates a split-duplex link")
      .def("create_split_duplex_link",
           py::overload_cast<const std::string&, const std::string&>(&simgrid::s4u::NetZone::create_split_duplex_link),
           "Creates a split-duplex link")
      .def("create_router", &simgrid::s4u::NetZone::create_router, "Create a router")
      .def_property("parent", &simgrid::s4u::NetZone::get_parent, &simgrid::s4u::NetZone::set_parent, "NetZone parent (r/w property).")
      // Keep `set_parent` method for backward compatibility.
      .def("set_parent", &simgrid::s4u::NetZone::set_parent, "Set the parent of this zone.")
      .def("get_children", &simgrid::s4u::NetZone::get_children, "Get all children of this zone.")
      .def("get_property", &simgrid::s4u::NetZone::get_property, "Retrieve NetZone property.")
      .def("get_properties", &simgrid::s4u::NetZone::get_properties, "Retrieve NetZone all properties.")
      .def("set_property", &simgrid::s4u::NetZone::set_property, "Add a property to this zone")
      .def("set_gateway", py::overload_cast<const simgrid::s4u::Host*>(&simgrid::s4u::NetZone::set_gateway),
           "Specify the gateway of this zone, to be used for inter-zone routes")
      .def("set_gateway", py::overload_cast<simgrid::kernel::routing::NetPoint*>(&simgrid::s4u::NetZone::set_gateway),
           "Specify the gateway of this zone, to be used for inter-zone routes")
      .def_property_readonly("netpoint", &simgrid::s4u::NetZone::get_netpoint,
                             "Retrieve the netpoint associated to this zone")
      .def("seal", &simgrid::s4u::NetZone::seal, "Seal this NetZone")
      .def_property_readonly("name", &simgrid::s4u::NetZone::get_name,
                             "The name of this network zone (read-only property).")
      .def_property_readonly("all_hosts", &simgrid::s4u::NetZone::get_all_hosts, "Retrieve all NetZone hosts (read-only property).")
      .def_property_readonly("host_count", &simgrid::s4u::NetZone::get_host_count, "Get NetZone host count (read-only property).")
      .def(
          "__repr__", [](const simgrid::s4u::NetZone net) { return "NetZone(" + net.get_name() + ")"; },
          "Textual representation of the NetZone");

  /* Class ClusterCallbacks */
  py::class_<simgrid::s4u::ClusterCallbacks>(m, "ClusterCallbacks", "Callbacks used to create cluster zones")
      .def(py::init<const std::function<simgrid::s4u::ClusterCallbacks::ClusterNetZoneCb>&,
                    const std::function<simgrid::s4u::ClusterCallbacks::ClusterLinkCb>&,
                    const std::function<simgrid::s4u::ClusterCallbacks::ClusterLinkCb>&>());

  /* Class FatTreeParams */
  py::class_<simgrid::s4u::FatTreeParams>(m, "FatTreeParams", "Parameters to create a Fat-Tree zone")
      .def(py::init<unsigned int, const std::vector<unsigned int>&, const std::vector<unsigned int>&,
                    const std::vector<unsigned int>&>());

  /* Class DragonflyParams */
  py::class_<simgrid::s4u::DragonflyParams>(m, "DragonflyParams", "Parameters to create a Dragonfly zone")
      .def(py::init<const std::pair<unsigned int, unsigned int>&, const std::pair<unsigned int, unsigned int>&,
                    const std::pair<unsigned int, unsigned int>&, unsigned int>());

  /* Class Host */
  py::class_<simgrid::s4u::Host, std::unique_ptr<Host, py::nodelete>> host(
      m, "Host", "Simulated host. See the C++ documentation for details.");
  host.def_static("by_name", &Host::by_name, py::arg("name"), "Retrieves a host from its name, or die")
      .def_static("by_name_or_null", &Host::by_name_or_null, py::arg("name"),
                  "Retrieves a host from its name or None if there is no such host")
      .def(
          "route_to",
          [](const simgrid::s4u::Host* h, const simgrid::s4u::Host* to) {
            auto* list = new std::vector<Link*>();
            double bw  = 0;
            h->route_to(to, *list, &bw);
            return make_tuple(list, bw);
          },
          "Retrieves the list of links and the bandwidth between two hosts.")
      .def(
          "set_speed_profile",
          [](Host* h, const std::string& profile, double period) {
            h->set_speed_profile(simgrid::kernel::profile::ProfileBuilder::from_string("", profile, period));
          },
          py::call_guard<py::gil_scoped_release>(),
          "Specify a profile modeling the external load according to an exhaustive list. "
          "Each line of the profile describes timed events as ``date ratio``. "
          "For example, the following content describes an host which computational speed is initially 1 (i.e, 100%) "
          "and then halved after 42 seconds:\n\n"
          ".. code-block:: python\n\n"
          "   \"\"\"\n"
          "   0 1.0\n"
          "   42 0.5\n"
          "   \"\"\"\n\n"
          ".. warning:: Don't get fooled: bandwidth and latency profiles of links contain absolute values,"
          " while speed profiles of hosts contain ratios.\n\n"
          "The second function parameter is the periodicity: the time to wait after the last event to start again over "
          "the list. Set it to -1 to not loop over.")
      .def(
          "set_state_profile",
          [](Host* h, const std::string& profile, double period) {
            h->set_state_profile(simgrid::kernel::profile::ProfileBuilder::from_string("", profile, period));
          },
          py::call_guard<py::gil_scoped_release>(),
          "Specify a profile modeling the churn. "
          "Each line of the profile describes timed events as ``date boolean``, where the boolean (0 or 1) tells "
          "whether the host is on. "
          "For example, the following content describes a link which turns off at t=1 and back on at t=2:\n\n"
          ".. code-block:: python\n\n"
          "   \"\"\"\n"
          "   1.0 0\n"
          "   2.0 1\n"
          "   \"\"\"\n\n"
          "The second function parameter is the periodicity: the time to wait after the last event to start again over "
          "the list. Set it to -1 to not loop over.")
      .def_property_readonly("pstate_count", &Host::get_pstate_count, "Retrieve the count of defined pstate levels")
      .def("pstate_speed", &Host::get_pstate_speed, "Retrieve the maximal speed at the given pstate")
      .def_property_readonly("netpoint", &Host::get_netpoint, "Retrieve the netpoint associated to this zone")
      .def_property_readonly("disks", &Host::get_disks, "The list of disks on this host (read-only).")
      .def_property_readonly("all_actors", &Host::get_all_actors, "Returns the list of all actors on the host")
      .def("get_disks", &Host::get_disks, "Retrieve the list of disks in this host")
      .def("get_disk_by_name", &Host::get_disk_by_name, "Retrieve the disk in this host by it's name")
      .def_property("core_count", &Host::get_core_count,
                    py::cpp_function(&Host::set_core_count, py::call_guard<py::gil_scoped_release>()),
                    "Manage the number of cores in the CPU")
      .def("set_coordinates", &Host::set_coordinates, py::call_guard<py::gil_scoped_release>(),
           "Set the coordinates of this host")
      .def("set_sharing_policy", &simgrid::s4u::Host::set_sharing_policy, py::call_guard<py::gil_scoped_release>(),
           "Describe how the CPU is shared", py::arg("policy"), py::arg("cb") = simgrid::s4u::NonLinearResourceCb())
      .def("create_disk", py::overload_cast<const std::string&, double, double>(&Host::create_disk),
           py::call_guard<py::gil_scoped_release>(), "Create a disk")
      .def("create_disk",
           py::overload_cast<const std::string&, const std::string&, const std::string&>(&Host::create_disk),
           py::call_guard<py::gil_scoped_release>(), "Create a disk")
      .def("get_property", &Host::get_property, "Get Host property")
      .def("get_properties", &Host::get_properties, "Get all Host properties")
      .def("set_property", &Host::set_property, "Set Host property")
      .def("set_properties", &Host::set_properties, "Set Host properties")
      .def_property("concurrency_limit", &Host::get_concurrency_limit,
                    py::cpp_function(&Host::set_concurrency_limit, py::call_guard<py::gil_scoped_release>()),
                    "Concurrency limit (read/write property).")
      .def("seal", &Host::seal, py::call_guard<py::gil_scoped_release>(), "Seal this host")
      .def("turn_off", &Host::turn_off, py::call_guard<py::gil_scoped_release>(), "Turn off this host")
      .def("turn_on", &Host::turn_on, py::call_guard<py::gil_scoped_release>(), "Turn on this host")
      .def_property("pstate", &Host::get_pstate,
                    py::cpp_function(&Host::set_pstate, py::call_guard<py::gil_scoped_release>()),
                    "The current pstate (read/write property).")
      .def_static("current", &Host::current, py::call_guard<py::gil_scoped_release>(),
                  "Retrieves the host on which the running actor is located.")
      .def_property_readonly("is_on", &Host::is_on, "Returns if that host is currently up and running (read-only property).")
      .def_property_readonly("actor_count", &Host::get_actor_count, "Returns actor count placed on this Host (read-only property).")
      .def_property_readonly("name", &Host::get_name, "The name of this host (read-only property).")
      .def_property_readonly("load", &Host::get_load,
                             "Returns the current computation load (in flops per second), NOT taking the external load "
                             "into account. This is the currently achieved speed (read-only property).")
      .def_property_readonly(
          "speed", &Host::get_speed,
          "The peak computing speed in flops/s at the current pstate, NOT taking the external load into account. "
          "This is the max potential speed (read-only property).")
      .def_property_readonly("available_speed", &Host::get_available_speed,
                             "Get the available speed ratio, between 0 and 1.\n"
                             "This accounts for external load (see :py:func:`set_speed_profile() "
                             "<simgrid.Host.set_speed_profile>`) (read-only property).")
      .def_static(
          "on_creation_cb",
          [](py::object cb) {
            cb.inc_ref(); // keep alive after return
            const py::gil_scoped_release gil_release;
            Host::on_creation_cb([cb_p = cb.ptr()](Host& h) {
              const py::gil_scoped_acquire py_context; // need a new context for callback
              try {
                const auto fun = py::reinterpret_borrow<py::function>(cb_p);
                fun(&h);
              } catch (const py::error_already_set& e) {
                xbt_die("Error while executing the on_creation lambda : %s", e.what());
              }
            });
          },
          "")
      .def(
          "__repr__", [](const Host* h) { return "Host(" + h->get_name() + ")"; },
          "Textual representation of the Host.");

  m.def("sg_host_load_plugin_init", [host]() {
    sg_host_load_plugin_init();

    static_cast<pybind11::class_<simgrid::s4u::Host, std::unique_ptr<simgrid::s4u::Host, pybind11::nodelete>>>(host)
        .def(
            "reset_load", [](const Host* h) { sg_host_load_reset(h); }, py::call_guard<py::gil_scoped_release>(),
            "Reset counters of the host load plugin for this host.")
        .def_property_readonly(
            "current_load", [](const Host* h) { return sg_host_get_current_load(h); }, "Current load of the host.")
        .def_property_readonly(
            "avg_load", [](const Host* h) { return sg_host_get_avg_load(h); }, "Average load of the host.")
        .def_property_readonly(
            "idle_time", [](const Host* h) { return sg_host_get_idle_time(h); }, "Idle time of the host")
        .def_property_readonly(
            "total_idle_time", [](const Host* h) { return sg_host_get_total_idle_time(h); },
            "Total idle time of the host.")
        .def_property_readonly(
            "computed_flops", [](const Host* h) { return sg_host_get_computed_flops(h); },
            "Computed flops of the host.");
  });

  py::enum_<simgrid::s4u::Host::SharingPolicy>(host, "SharingPolicy")
      .value("NONLINEAR", simgrid::s4u::Host::SharingPolicy::NONLINEAR)
      .value("LINEAR", simgrid::s4u::Host::SharingPolicy::LINEAR);

  /* Class Disk */
  py::class_<simgrid::s4u::Disk, std::unique_ptr<simgrid::s4u::Disk, py::nodelete>> disk(
      m, "Disk", "Simulated disk. See the C++ documentation for details.");
  disk.def("read", py::overload_cast<sg_size_t, double>(&simgrid::s4u::Disk::read, py::const_),
           py::call_guard<py::gil_scoped_release>(), "Read data from disk", py::arg("size"), py::arg("priority") = 1)
      .def("write", py::overload_cast<sg_size_t, double>(&simgrid::s4u::Disk::write, py::const_),
           py::call_guard<py::gil_scoped_release>(), "Write data in disk", py::arg("size"), py::arg("priority") = 1)
      .def("read_async", &simgrid::s4u::Disk::read_async, py::call_guard<py::gil_scoped_release>(),
           "Non-blocking read data from disk")
      .def("write_async", &simgrid::s4u::Disk::write_async, py::call_guard<py::gil_scoped_release>(),
           "Non-blocking write data in disk")
      .def_property("read_bandwidth", &simgrid::s4u::Disk::get_read_bandwidth, &simgrid::s4u::Disk::set_read_bandwidth,
                    py::call_guard<py::gil_scoped_release>(),
                    "The read bandwidth of this disk is the max speed at which read operations progress")
      .def_property("write_bandwidth", &simgrid::s4u::Disk::get_write_bandwidth,
                    &simgrid::s4u::Disk::set_write_bandwidth, py::call_guard<py::gil_scoped_release>(),
                    "The write bandwidth of this disk is the max speed at which write operations progress")
      .def_property(
          "concurrency_limit", &simgrid::s4u::Disk::get_concurrency_limit, &simgrid::s4u::Disk::set_concurrency_limit,
          py::call_guard<py::gil_scoped_release>(),
          "The concurrency limit of this disk is the max amound of concurrent accesses (extra ones are queued)")
      .def("set_sharing_policy", &simgrid::s4u::Disk::set_sharing_policy, py::call_guard<py::gil_scoped_release>(),
           "Set sharing policy for this disk", py::arg("op"), py::arg("policy"),
           py::arg("cb") = simgrid::s4u::NonLinearResourceCb())
      .def("seal", &simgrid::s4u::Disk::seal, py::call_guard<py::gil_scoped_release>(), "Seal this disk")
      .def_property_readonly("name", &simgrid::s4u::Disk::get_name, "The name of this disk (read-only property).")
      .def(
          "__repr__", [](const Disk* d) { return "Disk(" + d->get_name() + ")"; },
          "Textual representation of the Disk");
  py::enum_<simgrid::s4u::Disk::SharingPolicy>(disk, "SharingPolicy")
      .value("NONLINEAR", simgrid::s4u::Disk::SharingPolicy::NONLINEAR)
      .value("LINEAR", simgrid::s4u::Disk::SharingPolicy::LINEAR);
  py::enum_<simgrid::s4u::Disk::Operation>(disk, "Operation")
      .value("READ", simgrid::s4u::Disk::Operation::READ)
      .value("WRITE", simgrid::s4u::Disk::Operation::WRITE)
      .value("READWRITE", simgrid::s4u::Disk::Operation::READWRITE);

  /* Class NetPoint */
  py::class_<simgrid::kernel::routing::NetPoint, std::unique_ptr<simgrid::kernel::routing::NetPoint, py::nodelete>>
      netpoint(m, "NetPoint", "NetPoint object");

  /* Class Link */
  py::class_<Link, std::unique_ptr<Link, py::nodelete>> link(m, "Link",
                                                             "Network link. See the C++ documentation for details.");
  link.def("set_latency", py::overload_cast<const std::string&>(&Link::set_latency),
           py::call_guard<py::gil_scoped_release>(),
           "Set the latency as a string. Accepts values with units, such as ‘1s’ or ‘7ms’.\nFull list of accepted "
           "units: w (week), d (day), h, s, ms, us, ns, ps.")
      .def("set_latency", py::overload_cast<double>(&Link::set_latency), py::call_guard<py::gil_scoped_release>(),
           "Set the latency as a float (in seconds).")
      // Keep `set_bandwidth` for backward compatibility.
      .def("set_bandwidth", &Link::set_bandwidth, py::call_guard<py::gil_scoped_release>(),
           "Set the bandwidth (in byte per second).")
      .def(
          "set_bandwidth_profile",
          [](Link* l, const std::string& profile, double period) {
            l->set_bandwidth_profile(simgrid::kernel::profile::ProfileBuilder::from_string("", profile, period));
          },
          py::call_guard<py::gil_scoped_release>(),
          "Specify a profile modeling the external load according to an exhaustive list. "
          "Each line of the profile describes timed events as ``date bandwidth`` (in bytes per second). "
          "For example, the following content describes a link which bandwidth changes to 40 Mb/s at t=4, and to 6 "
          "Mb/s at t=8:\n\n"
          ".. code-block:: python\n\n"
          "   \"\"\"\n"
          "   4.0 40000000\n"
          "   8.0 60000000\n"
          "   \"\"\"\n\n"
          ".. warning:: Don't get fooled: bandwidth and latency profiles of links contain absolute values,"
          " while speed profiles of hosts contain ratios.\n\n"
          "The second function parameter is the periodicity: the time to wait after the last event to start again over "
          "the list. Set it to -1 to not loop over.")
      .def(
          "set_latency_profile",
          [](Link* l, const std::string& profile, double period) {
            l->set_latency_profile(simgrid::kernel::profile::ProfileBuilder::from_string("", profile, period));
          },
          py::call_guard<py::gil_scoped_release>(),
          "Specify a profile modeling the external load according to an exhaustive list. "
          "Each line of the profile describes timed events as ``date latency`` (in seconds). "
          "For example, the following content describes a link which latency changes to 1ms (0.001s) at t=1, and to 2s "
          "at t=2:\n\n"
          ".. code-block:: python\n\n"
          "   \"\"\"\n"
          "   1.0 0.001\n"
          "   2.0 2\n"
          "   \"\"\"\n\n"
          ".. warning:: Don't get fooled: bandwidth and latency profiles of links contain absolute values,"
          " while speed profiles of hosts contain ratios.\n\n"
          "The second function parameter is the periodicity: the time to wait after the last event to start again over "
          "the list. Set it to -1 to not loop over.")
      .def(
          "set_state_profile",
          [](Link* l, const std::string& profile, double period) {
            l->set_state_profile(simgrid::kernel::profile::ProfileBuilder::from_string("", profile, period));
          },
          "Specify a profile modeling the churn. "
          "Each line of the profile describes timed events as ``date boolean``, where the boolean (0 or 1) tells "
          "whether the link is on. "
          "For example, the following content describes a link which turns off at t=1 and back on at t=2:\n\n"
          ".. code-block:: python\n\n"
          "   \"\"\"\n"
          "   1.0 0\n"
          "   2.0 1\n"
          "   \"\"\"\n\n"
          "The second function parameter is the periodicity: the time to wait after the last event to start again over "
          "the list. Set it to -1 to not loop over.")

      .def("turn_on", &Link::turn_on, py::call_guard<py::gil_scoped_release>(), "Turns the link on.")
      .def("turn_off", &Link::turn_off, py::call_guard<py::gil_scoped_release>(), "Turns the link off.")
      .def("is_on", &Link::is_on, "Check whether the link is on.")
      .def_property_readonly("is_used", &Link::is_used, "Check if the link is used (at least one flow uses the link).")
      .def("get_sharing_policy", &Link::get_sharing_policy, "Retrieve link sharing policy.")
      .def("set_sharing_policy", &Link::set_sharing_policy, py::call_guard<py::gil_scoped_release>(),
           "Set sharing policy for this link")
      .def_property("concurrency_limit", &Link::get_concurrency_limit,
                    py::cpp_function(&Link::set_concurrency_limit, py::call_guard<py::gil_scoped_release>()),
                    "Concurrency limit (read/write property).")
      // Keep `set_concurrency_limit` method for backward compatibility.
      .def("set_concurrency_limit", &Link::set_concurrency_limit, py::call_guard<py::gil_scoped_release>(),
           "Set concurrency limit for this link")
      .def("set_host_wifi_rate", &Link::set_host_wifi_rate, py::call_guard<py::gil_scoped_release>(),
           "Set level of communication speed of given host on this Wi-Fi link")
      .def_static("by_name", &Link::by_name, "Retrieves a Link from its name, or dies")
      .def_static("by_name_or_null", &Link::by_name_or_null,
                 "Retrieve a Link by its name, or None if it does not exist in the platform.")
      .def("seal", &Link::seal, py::call_guard<py::gil_scoped_release>(), "Seal this link")
      .def_property_readonly("name", &Link::get_name, "The name of this link")
      .def_property("bandwidth", &Link::get_bandwidth,
                    py::cpp_function(&Link::set_bandwidth, py::call_guard<py::gil_scoped_release>()),
                    "The bandwidth (in bytes per second) (r/w property).")
      .def_property_readonly("latency", &Link::get_latency, "The latency (in seconds) (read-only property).")
      .def_property_readonly("load", &Link::get_load, "Returns the current load (in bytes per second) (read-only property).")
      .def("get_property", &Link::get_property, "Retrieve link property.")
      .def("get_properties", &Link::get_properties, "Retrieve link all properties.")
      .def("set_property", &Link::set_property, "Set link property.")
      .def("set_properties", &Link::set_properties, "Set multiple properties.")
      .def(
          "__repr__", [](const Link* l) { return "Link(" + l->get_name() + ")"; },
          "Textual representation of the Link");
  py::enum_<Link::SharingPolicy>(link, "SharingPolicy")
      .value("NONLINEAR", Link::SharingPolicy::NONLINEAR,
             "This policy takes a callback that specifies the maximal capacity as a function of the number of usage. "
             "See the examples with 'degradation' in their name.")
      .value("WIFI", Link::SharingPolicy::WIFI, "Pseudo-sharing policy requesting wifi-specific sharing.")
      .value("SPLITDUPLEX", Link::SharingPolicy::SPLITDUPLEX,
             "Each link is split in 2, UP and DOWN, one per direction. These links are SHARED.")
      .value("SHARED", Link::SharingPolicy::SHARED,
             "The bandwidth is shared between all comms using that link, regardless of their direction.")
      .value("FATPIPE", Link::SharingPolicy::FATPIPE,
             "Each comm can use the link fully, with no sharing (only a maximum). This is intended to represent the "
             "backbone links that cannot be saturated by concurrent links, but have a maximal bandwidth.");

  /* Class LinkInRoute */
  py::class_<simgrid::s4u::LinkInRoute> linkinroute(m, "LinkInRoute", "Abstraction to add link in routes");
  linkinroute.def(py::init<const Link*>());
  linkinroute.def(py::init<const Link*, simgrid::s4u::LinkInRoute::Direction>());
  py::enum_<simgrid::s4u::LinkInRoute::Direction>(linkinroute, "Direction")
      .value("UP", simgrid::s4u::LinkInRoute::Direction::UP)
      .value("DOWN", simgrid::s4u::LinkInRoute::Direction::DOWN)
      .value("NONE", simgrid::s4u::LinkInRoute::Direction::NONE);

  /* Class Split-Duplex Link */
  py::class_<simgrid::s4u::SplitDuplexLink, Link, std::unique_ptr<simgrid::s4u::SplitDuplexLink, py::nodelete>>(
      m, "SplitDuplexLink", "Network split-duplex link")
      .def_property_readonly("link_up", &simgrid::s4u::SplitDuplexLink::get_link_up, "Get link direction up")
      .def_property_readonly("link_down", &simgrid::s4u::SplitDuplexLink::get_link_down, "Get link direction down");

  /* Class Mailbox */
  py::class_<simgrid::s4u::Mailbox, std::unique_ptr<Mailbox, py::nodelete>>(
      m, "Mailbox", "Mailbox. See the C++ documentation for details.")
      .def(
          "__repr__", [](const Mailbox* self) { return "Mailbox(" + self->get_name() + ")"; },
          "Textual representation of the Mailbox")
      .def_static("by_name", &Mailbox::by_name, py::call_guard<py::gil_scoped_release>(), py::arg("name"),
                  "Retrieve a Mailbox from its name")
      .def_property_readonly("name", &Mailbox::get_name, "The name of that mailbox (read-only property).")
      .def_property_readonly("ready", &Mailbox::ready,
                             "Check if there is a communication ready to be consumed from a mailbox.")
      .def(
          "put",
          [](Mailbox* self, py::object data, uint64_t size, double timeout) {
            auto* data_ptr = data.inc_ref().ptr();
            const py::gil_scoped_release gil_release;
            self->put(data_ptr, size, timeout);
          },
          "Blocking data transmission with a timeout")
      .def(
          "put",
          [](Mailbox* self, py::object data, uint64_t size) {
            auto* data_ptr = data.inc_ref().ptr();
            const py::gil_scoped_release gil_release;
            self->put(data_ptr, size);
          },
          "Blocking data transmission")
      .def(
          "put_async",
          [](Mailbox* self, py::object data, uint64_t size) {
            auto* data_ptr = data.inc_ref().ptr();
            const py::gil_scoped_release gil_release;
            return self->put_async(data_ptr, size);
          },
          "Non-blocking data transmission")
      .def(
          "put_init",
          [](Mailbox* self, py::object data, uint64_t size) {
            auto* data_ptr = data.inc_ref().ptr();
            const py::gil_scoped_release gil_release;
            return self->put_init(data_ptr, size);
          },
          "Creates (but don’t start) a data transmission to that mailbox.")
      .def(
          "get", [](Mailbox* self) { return py::reinterpret_steal<py::object>(self->get<PyObject>()); },
          py::call_guard<py::gil_scoped_release>(), "Blocking data reception")
      .def(
          "get_async", [](Mailbox* self) -> CommPtr { return self->get_async(); },
          py::call_guard<py::gil_scoped_release>(),
          "Non-blocking data reception. Use data.get() to get the python object after the communication has finished")
      .def("set_receiver", &Mailbox::set_receiver, py::call_guard<py::gil_scoped_release>(),
           "Sets the actor as permanent receiver");

  /* class Activity */
  py::class_<Activity, ActivityPtr>(m, "Activity", "Activity. See the C++ documentation for details.");

  /* Class Comm */
  py::class_<Comm, CommPtr, Activity>(m, "Comm", "Communication. See the C++ documentation for details.")
      .def_property_readonly("dst_data_size", &Comm::get_dst_data_size, py::call_guard<py::gil_scoped_release>(),
                             "Retrieve the size of the received data.")
      .def_property_readonly("mailbox", &Comm::get_mailbox, py::call_guard<py::gil_scoped_release>(),
                             "Retrieve the mailbox on which this comm acts.")
      .def_property_readonly("sender", &Comm::get_sender, py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("state_str", &Comm::get_state_str, py::call_guard<py::gil_scoped_release>(),
                             "Retrieve the Comm state as string")
      .def_property_readonly("remaining", &Comm::get_remaining, py::call_guard<py::gil_scoped_release>(),
                             "Remaining amount of work that this Comm entails")
      .def_property_readonly("start_time", &Comm::get_start_time, py::call_guard<py::gil_scoped_release>(),
                             "Time at which this Comm started")
      .def_property_readonly("finish_time", &Comm::get_finish_time, py::call_guard<py::gil_scoped_release>(),
                             "Time at which this Comm finished")
      .def_property_readonly("is_suspended", &Comm::is_suspended, py::call_guard<py::gil_scoped_release>(),
                             "Whether this Comm is suspended")
      .def("set_tracing_category", &Comm::set_tracing_category, py::call_guard<py::gil_scoped_release>(), py::arg("category"),
           "Set a user-defined tracing category.")
      .def("set_payload_size", &Comm::set_payload_size, py::call_guard<py::gil_scoped_release>(), py::arg("bytes"),
           "Specify the amount of bytes which exchange should be simulated.")
      .def("set_rate", &Comm::set_rate, py::call_guard<py::gil_scoped_release>(), py::arg("rate"),
           "Sets the maximal communication rate (in byte/sec). Must be done before start")
      .def("cancel", &Comm::cancel, py::call_guard<py::gil_scoped_release>(),
           py::return_value_policy::reference_internal, "Cancel the activity.")
      .def("start", &Comm::start, py::call_guard<py::gil_scoped_release>(), py::return_value_policy::reference_internal,
           "Starts a previously created activity. This function is optional: you can call wait() even if you didn't "
           "call start()")
      .def("suspend", &Comm::suspend, py::call_guard<py::gil_scoped_release>(),
           py::return_value_policy::reference_internal, "Suspend the activity.")
      .def("resume", &Comm::resume, py::call_guard<py::gil_scoped_release>(),
           py::return_value_policy::reference_internal, "Resume the activity.")
      .def("test", &Comm::test, py::call_guard<py::gil_scoped_release>(),
           "Test whether the communication is terminated.")
      .def("wait", &Comm::wait, py::call_guard<py::gil_scoped_release>(),
           "Block until the completion of that communication.")
      .def("wait_for", &Comm::wait_for, py::call_guard<py::gil_scoped_release>(), py::arg("timeout"),
           "Block until the completion of that communication, or raises TimeoutException after the specified timeout.")
      .def("wait_until", &Comm::wait_until, py::call_guard<py::gil_scoped_release>(), py::arg("time_limit"),
           "Block until the completion of that communication, or raises TimeoutException after the specified time.")
      .def(
          "get_payload",
          [](const Comm* self) { return py::reinterpret_steal<py::object>((PyObject*)self->get_payload()); },
          py::call_guard<py::gil_scoped_release>(),
          "Retrieve the message's payload of a get_async. You cannot call this until after the comm termination.")
      .def("detach", py::overload_cast<>(&Comm::detach), py::return_value_policy::reference_internal,
           py::call_guard<py::gil_scoped_release>(),
           "Start the comm, and ignore its result. It can be completely forgotten after that.")
      .def_static("sendto", &Comm::sendto, py::call_guard<py::gil_scoped_release>(), py::arg("from"), py::arg("to"),
                  py::arg("simulated_size_in_bytes"), "Do a blocking communication between two arbitrary hosts.")
      .def_static("sendto_init", py::overload_cast<Host*, Host*>(&Comm::sendto_init),
                  py::call_guard<py::gil_scoped_release>(), py::arg("from"), py::arg("to"),
                  "Creates a communication between the two given hosts, bypassing the mailbox mechanism.")
      .def_static("sendto_async", &Comm::sendto_async, py::call_guard<py::gil_scoped_release>(), py::arg("from"),
                  py::arg("to"), py::arg("simulated_size_in_bytes"),
                  "Do a blocking communication between two arbitrary hosts.\n\nThis initializes a communication that "
                  "completely bypass the mailbox and actors mechanism. There is really no limit on the hosts involved. "
                  "In particular, the actor does not have to be on one of the involved hosts.");

  /* Class Io */
  py::class_<simgrid::s4u::Io, simgrid::s4u::IoPtr, Activity>(m, "Io",
                                                              "I/O activities. See the C++ documentation for details.")
      .def("set_tracing_category", &simgrid::s4u::Io::set_tracing_category, py::call_guard<py::gil_scoped_release>(), py::arg("category"),
           "Set a user-defined tracing category.")
      .def("test", &simgrid::s4u::Io::test, py::call_guard<py::gil_scoped_release>(),
           "Test whether the I/O is terminated.")
      .def("wait", &simgrid::s4u::Io::wait, py::call_guard<py::gil_scoped_release>(),
           "Block until the completion of that I/O operation");

  /* Class Exec */
  py::class_<simgrid::s4u::Exec, simgrid::s4u::ExecPtr, Activity>(m, "Exec",
                                                                  "Execution. See the C++ documentation for details.")
      .def_property_readonly("remaining", &simgrid::s4u::Exec::get_remaining, py::call_guard<py::gil_scoped_release>(),
                             "Amount of flops that remain to be computed until completion (read-only property).")
      .def_property_readonly("remaining_ratio", &simgrid::s4u::Exec::get_remaining_ratio,
                             py::call_guard<py::gil_scoped_release>(),
                             "Amount of work remaining until completion from 0 (completely done) to 1 (nothing done "
                             "yet) (read-only property).")
      .def_property("host", &simgrid::s4u::Exec::get_host, &simgrid::s4u::Exec::set_host,
                    "Host on which this execution runs. Only the first host is returned for parallel executions. "
                    "Changing this value migrates the execution.")
      .def_property_readonly("is_suspended", &simgrid::s4u::Exec::is_suspended,
                             py::call_guard<py::gil_scoped_release>(), "Whether this Exec is suspended")
      .def("set_tracing_category", &simgrid::s4u::Exec::set_tracing_category, py::call_guard<py::gil_scoped_release>(), py::arg("category"),
           "Set a user-defined tracing category.")
      .def("test", &simgrid::s4u::Exec::test, py::call_guard<py::gil_scoped_release>(),
           "Test whether the execution is terminated.")
      .def("cancel", &simgrid::s4u::Exec::cancel, py::call_guard<py::gil_scoped_release>(), "Cancel that execution.")
      .def("start", &simgrid::s4u::Exec::start, py::call_guard<py::gil_scoped_release>(), "Start that execution.")
      .def("suspend", &simgrid::s4u::Exec::suspend, py::call_guard<py::gil_scoped_release>(), "Suspend that execution.")
      .def("resume", &simgrid::s4u::Exec::resume, py::call_guard<py::gil_scoped_release>(), "Resume that execution.")
      .def("wait", &simgrid::s4u::Exec::wait, py::call_guard<py::gil_scoped_release>(),
           "Block until the completion of that execution.")
      .def("wait_for", &simgrid::s4u::Exec::wait_for, py::call_guard<py::gil_scoped_release>(), py::arg("timeout"),
           "Block until the completion of that activity, or raises TimeoutException after the specified timeout.");

  /* Class Semaphore */
  py::class_<Semaphore, SemaphorePtr>(m, "Semaphore",
                                      "A classical semaphore, but blocking in the simulation world. See the C++ "
                                      "documentation for details.")
      .def(py::init<>(&Semaphore::create), py::call_guard<py::gil_scoped_release>(), py::arg("capacity"),
           "Semaphore constructor.")
      .def("acquire", &Semaphore::acquire, py::call_guard<py::gil_scoped_release>(),
           "Acquire on the semaphore object with no timeout. Blocks until the semaphore is acquired.")
      .def("acquire_timeout", &Semaphore::acquire_timeout, py::call_guard<py::gil_scoped_release>(), py::arg("timeout"),
           "Acquire on the semaphore object with no timeout. Blocks until the semaphore is acquired or return "
           "true if it has not been acquired after the specified timeout.")
      .def("release", &Semaphore::release, py::call_guard<py::gil_scoped_release>(), "Release the semaphore.")
      .def_property_readonly("capacity", &Semaphore::get_capacity, py::call_guard<py::gil_scoped_release>(),
                             "Get the semaphore capacity.")
      .def_property_readonly("would_block", &Semaphore::would_block, py::call_guard<py::gil_scoped_release>(),
                             "Check whether trying to acquire the semaphore would block (in other word, checks whether "
                             "this semaphore has capacity).")
      // Allow semaphores to be automatically acquired/released with a context manager: `with semaphore: ...`
      .def("__enter__", &Semaphore::acquire, py::call_guard<py::gil_scoped_release>())
      .def("__exit__",
           [](Semaphore* self, const py::object&, const py::object&, const py::object&) { self->release(); });

  /* Class Mutex */
  py::class_<Mutex, MutexPtr>(m, "Mutex",
                              "A classical mutex, but blocking in the simulation world."
                              "See the C++ documentation for details.")
      .def(py::init<>(&Mutex::create), py::call_guard<py::gil_scoped_release>(),
           "Mutex constructor (pass True as a parameter to get a recursive Mutex).", py::arg("recursive") = false)
      .def("lock", &Mutex::lock, py::call_guard<py::gil_scoped_release>(), "Block until the mutex is acquired.")
      .def("try_lock", &Mutex::try_lock, py::call_guard<py::gil_scoped_release>(),
           "Try to acquire the mutex. Return true if the mutex was acquired, false otherwise.")
      .def("unlock", &Mutex::unlock, py::call_guard<py::gil_scoped_release>(), "Release the mutex.")
      // Allow mutexes to be automatically acquired/released with a context manager: `with mutex: ...`
      .def("__enter__", &Mutex::lock, py::call_guard<py::gil_scoped_release>())
      .def(
          "__exit__", [](Mutex* self, const py::object&, const py::object&, const py::object&) { self->unlock(); },
          py::call_guard<py::gil_scoped_release>());

  /* Class Barrier */
  py::class_<Barrier, BarrierPtr>(m, "Barrier", "A classical barrier, but blocking in the simulation world.")
      .def(py::init<>(&Barrier::create), py::call_guard<py::gil_scoped_release>(), py::arg("expected_actors"),
           "Barrier constructor.")
      .def("wait", &Barrier::wait, py::call_guard<py::gil_scoped_release>(),
           "Blocks into the barrier. Every waiting actors will be unlocked once the expected amount of actors reaches "
           "the barrier.");

  /* Class Actor */
  py::class_<simgrid::s4u::Actor, ActorPtr>(m, "Actor",
                                            "An actor is an independent stream of execution in your distributed "
                                            "application. See the C++ documentation for details.")
      .def(
          "create",
          [](const std::string& name, Host* h, py::object fun, py::args args) {
            fun.inc_ref();  // keep alive after return
            args.inc_ref(); // keep alive after return
            const py::gil_scoped_release gil_release;
            return simgrid::s4u::Actor::create(name, h, [fun_p = fun.ptr(), args_p = args.ptr()]() {
              const py::gil_scoped_acquire py_context;
              try {
                const auto fun  = py::reinterpret_borrow<py::object>(fun_p);
                const auto args = py::reinterpret_borrow<py::args>(args_p);
                fun(*args);
              } catch (const py::error_already_set& ex) {
                if (ex.matches(pyForcefulKillEx)) {
                  XBT_VERB("Actor killed");
                  simgrid::ForcefulKillException::do_throw(); // Forward that ForcefulKill exception
                }
                throw;
              }
            });
          },
          "Create an actor from a function or an object. See the :ref:`example <s4u_ex_actors_create>`.")
      .def_property(
          "host", &Actor::get_host, py::cpp_function(&Actor::set_host, py::call_guard<py::gil_scoped_release>()),
          "The host on which this actor is located. Changing this value migrates the actor.\n\n"
          "If the actor is currently blocked on an execution activity, the activity is also migrated to the new host. "
          "If it’s blocked on another kind of activity, an error is raised as the mandated code is not written yet. "
          "Please report that bug if you need it.\n\n"
          "Asynchronous activities started by the actor are not migrated automatically, so you have to take care of "
          "this yourself (only you knows which ones should be migrated). ")
      .def_property_readonly("name", &Actor::get_cname, "The name of this actor (read-only property).")
      .def_property_readonly("pid", &Actor::get_pid, "The PID (unique identifier) of this actor (read-only property).")
      .def_property_readonly("ppid", &Actor::get_ppid,
                             "The PID (unique identifier) of the actor that created this one (read-only property).")
      .def_static("by_pid", &Actor::by_pid, py::arg("pid"), "Retrieve an actor by its PID")
      .def("set_auto_restart", &Actor::set_auto_restart, py::call_guard<py::gil_scoped_release>(),
           "Specify whether the actor shall restart when its host reboots.")
      .def("daemonize", &Actor::daemonize, py::call_guard<py::gil_scoped_release>(),
           "This actor will be automatically terminated when the last non-daemon actor finishes (more info in the C++ "
           "documentation).")
      .def("is_daemon", &Actor::is_daemon,
           "Returns True if that actor is a daemon and will be terminated automatically when the last non-daemon actor "
           "terminates.")
      .def("join", py::overload_cast<double>(&Actor::join, py::const_), py::call_guard<py::gil_scoped_release>(),
           "Wait for the actor to finish (more info in the C++ documentation).", py::arg("timeout") = -1)
      .def("kill", &Actor::kill, py::call_guard<py::gil_scoped_release>(), "Kill that actor")
      .def("self", &Actor::self, "Retrieves the current actor.")
      .def("is_suspended", &Actor::is_suspended, "Returns True if that actor is currently suspended.")
      .def("suspend", &Actor::suspend, py::call_guard<py::gil_scoped_release>(),
           "Suspend that actor, that is blocked until resume()ed by another actor.")
      .def("resume", &Actor::resume, py::call_guard<py::gil_scoped_release>(),
           "Resume that actor, that was previously suspend()ed.")
      .def_static("kill_all", &Actor::kill_all, py::call_guard<py::gil_scoped_release>(),
                  "Kill all actors but the caller.")
      .def(
          "__repr__", [](const ActorPtr a) { return "Actor(" + a->get_name() + ")"; },
          "Textual representation of the Actor");

  /* Enum Class IoOpType */
  py::enum_<simgrid::s4u::Io::OpType>(m, "IoOpType")
      .value("READ", simgrid::s4u::Io::OpType::READ)
      .value("WRITE", simgrid::s4u::Io::OpType::WRITE);

  /* Class Task */
  py::class_<Task, TaskPtr>(m, "Task", "Task. See the C++ documentation for details.")
      .def_static(
          "on_start_cb",
          [](py::object cb) {
            cb.inc_ref(); // keep alive after return
            const py::gil_scoped_release gil_release;
            Task::on_start_cb([cb_p = cb.ptr()](Task* op) {
              const py::gil_scoped_acquire py_context; // need a new context for callback
              py::reinterpret_borrow<py::function>(cb_p)(op);
            });
          },
          "Add a callback called when each task starts.")
      .def_static(
          "on_completion_cb",
          [](py::object cb) {
            cb.inc_ref(); // keep alive after return
            const py::gil_scoped_release gil_release;
            Task::on_completion_cb([cb_p = cb.ptr()](Task* op) {
              const py::gil_scoped_acquire py_context; // need a new context for callback
              py::reinterpret_borrow<py::function>(cb_p)(op);
            });
          },
          "Add a callback called when each task ends.")
      .def_property_readonly("name", &Task::get_name, "The name of this task (read-only).")
      .def_property_readonly("successors", &Task::get_successors, "The successors of this task (read-only).")
      .def_property("amount", &Task::get_amount, &Task::set_amount, "The amount of work to do for this task.")
      .def(
          "get_count", [](const TaskPtr t) { return t->get_count("instance_0"); },
          "The execution count of this task instance_0.")
      .def(
          "get_count", [](const TaskPtr t, const std::string& instance) { return t->get_count(instance); },
          "The execution count of this task instance.")
      .def("enqueue_firings", py::overload_cast<int>(&Task::enqueue_firings), py::call_guard<py::gil_scoped_release>(),
           py::arg("n"), "Enqueue firings for this task.")
      .def("add_successor", py::overload_cast<TaskPtr>(&Task::add_successor), py::call_guard<py::gil_scoped_release>(),
           py::arg("op"), "Add a successor to this task.")
      .def("remove_successor", py::overload_cast<TaskPtr>(&Task::remove_successor),
           py::call_guard<py::gil_scoped_release>(), py::arg("op"), "Remove a successor of this task.")
      .def("remove_all_successors", &Task::remove_all_successors, py::call_guard<py::gil_scoped_release>(),
           "Remove all successors of this task.")
      .def("on_this_start_cb", py::overload_cast<const std::function<void(Task*)>&>(&Task::on_this_start_cb),
           py::arg("func"), "Add a callback called when this task starts.")
      .def("on_this_completion_cb", py::overload_cast<const std::function<void(Task*)>&>(&Task::on_this_completion_cb),
           py::arg("func"), "Add a callback called when this task ends.")
      .def(
          "__repr__", [](const TaskPtr op) { return "Task(" + op->get_name() + ")"; },
          "Textual representation of the Task");

  /* Class CommTask */
  py::class_<CommTask, CommTaskPtr, Task>(m, "CommTask", "Communication Task. See the C++ documentation for details.")
      .def_static("init", py::overload_cast<const std::string&>(&CommTask::init),
                  py::call_guard<py::gil_scoped_release>(), py::arg("name"), "CommTask constructor")
      .def_static("init", py::overload_cast<const std::string&, double, Host*, Host*>(&CommTask::init),
                  py::call_guard<py::gil_scoped_release>(), py::arg("name"), py::arg("bytes"), py::arg("source"),
                  py::arg("destination"), "CommTask constructor")
      .def_property("source", &CommTask::get_source, &CommTask::set_source, "The source of the communication.")
      .def_property("destination", &CommTask::get_destination, &CommTask::set_destination,
                    "The destination of the communication.")
      .def_property("bytes", &CommTask::get_bytes, &CommTask::set_bytes, "The amount of bytes to send.")
      .def(
          "__repr__", [](const CommTaskPtr c) { return "CommTask(" + c->get_name() + ")"; },
          "Textual representation of the CommTask");

  /* Class ExecTask */
  py::class_<ExecTask, ExecTaskPtr, Task>(m, "ExecTask", "Execution Task. See the C++ documentation for details.")
      .def_static("init", py::overload_cast<const std::string&>(&ExecTask::init),
                  py::call_guard<py::gil_scoped_release>(), py::arg("name"), "ExecTask constructor")
      .def_static("init", py::overload_cast<const std::string&, double, Host*>(&ExecTask::init),
                  py::call_guard<py::gil_scoped_release>(), py::arg("name"), py::arg("flops"), py::arg("host"),
                  "ExecTask constructor.")
      .def_property("host", &ExecTask::get_host, &ExecTask::set_host, "The host of the execution.")
      .def_property("flops", &ExecTask::get_flops, &ExecTask::set_flops, "The amount of flops to execute.")
      .def(
          "__repr__", [](const ExecTaskPtr e) { return "ExecTask(" + e->get_name() + ")"; },
          "Textual representation of the ExecTask");

  /* Class IoTask */
  py::class_<IoTask, IoTaskPtr, Task>(m, "IoTask", "IO Task. See the C++ documentation for details.")
      .def_static("init", py::overload_cast<const std::string&>(&IoTask::init),
                  py::call_guard<py::gil_scoped_release>(), py::arg("name"), "IoTask constructor")
      .def_static("init", py::overload_cast<const std::string&, double, Disk*, Io::OpType>(&IoTask::init),
                  py::call_guard<py::gil_scoped_release>(), py::arg("name"), py::arg("bytes"), py::arg("disk"),
                  py::arg("type"), "IoTask constructor.")
      .def_property("disk", &IoTask::get_disk, &IoTask::set_disk, "The disk of the IO.")
      .def_property("bytes", &IoTask::get_bytes, &IoTask::set_bytes, "The amount of bytes to process.")
      .def_property("type", &IoTask::get_op_type, &IoTask::set_op_type, "The type of IO.")
      .def(
          "__repr__", [](const IoTaskPtr io) { return "IoTask(" + io->get_name() + ")"; },
          "Textual representation of the IoTask");

  /* Class ActivitySet */
  py::class_<ActivitySet, ActivitySetPtr>(m, "ActivitySet", "ActivitySet. See the C++ documentation for details.")
      .def(py::init([](const std::vector<simgrid::s4u::ActivityPtr>& activities) {
             auto* ret = new ActivitySet();
             for (auto a : activities)
               ret->push(a);
             return ActivitySetPtr(ret);
           }),
           "The constructor should take the parameters from the command line, as is ")
      .def(py::init([]() { return ActivitySetPtr(new ActivitySet()); }),
           "The constructor should take the parameters from the command line, as is ")

      .def("push", &ActivitySet::push, py::call_guard<py::gil_scoped_release>(), py::arg("activity"),
           "Add an activity to the set")
      .def("erase", &ActivitySet::erase, py::call_guard<py::gil_scoped_release>(), py::arg("activity"),
           "Remove that activity from the set")
      .def_property_readonly("size", &ActivitySet::size, "Count of activities in the set")
      .def("empty", &ActivitySet::empty, "Returns whether the set is empty")
      .def("has_failed_activities", &ActivitySet::has_failed_activities,
           "Returns whether there is any failed activities")
      .def("get_failed_activity", &ActivitySet::get_failed_activity, "Returns a failed activity from the set, or None")

      .def("wait_all_for", &ActivitySet::wait_all_for, py::call_guard<py::gil_scoped_release>(), py::arg("timeout"),
           "Wait for the completion of all activities in the set, but not longer than the provided timeout")
      .def("wait_all", &ActivitySet::wait_all, py::call_guard<py::gil_scoped_release>(),
           "Wait for the completion of all activities in the set, endlessly")
      .def("test_any", &ActivitySet::test_any, py::call_guard<py::gil_scoped_release>(),
           "Returns the first terminated activity if any, or None if no activity is terminated")
      .def("wait_any_for", &ActivitySet::wait_any_for, py::call_guard<py::gil_scoped_release>(), py::arg("timeout"),
           "Wait for the completion of one activity in the set, but not longer than the provided timeout")
      .def("wait_any", &ActivitySet::wait_any, py::call_guard<py::gil_scoped_release>(),
           "Wait for the completion of one activity in the set, endlessly")

      .def(
          "__repr__", [](const ActivitySetPtr) { return "ActivitySet([...])"; },
          "Textual representation of the ActivitySet");
#ifdef SIMGRID_PYTHON_SMPI
  SMPI_bindings(m);
#endif
}
