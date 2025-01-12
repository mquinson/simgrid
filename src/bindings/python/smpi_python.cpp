/* Copyright (c) 2018-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */
#include <pybind11/pybind11.h> // Must come before our own stuff

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <string>
#include <vector>

#include <smpi/smpi.h>
#include <smpi_comm.hpp>
#include <smpi_datatype.hpp>
#include <smpi_op.hpp>

#include <simgrid/Exception.hpp>
#include <simgrid/s4u/Host.hpp>

#define BIND_ALL_TYPES(module, name, function, ...)                                                                    \
  module.def(name, function<uint8_t>, ##__VA_ARGS__);                                                                  \
  module.def(name, function<uint16_t>, ##__VA_ARGS__);                                                                 \
  module.def(name, function<int>, ##__VA_ARGS__);                                                                      \
  module.def(name, function<double>, ##__VA_ARGS__);                                                                   \
  module.def(name, function<float>, ##__VA_ARGS__);                                                                    \
  module.def(name, function<long>, ##__VA_ARGS__);                                                                     \
  module.def(name, function<long long>, ##__VA_ARGS__);

namespace py = pybind11;
using simgrid::s4u::Host;
XBT_LOG_NEW_DEFAULT_CATEGORY(smpi_python, "python");

namespace {
template <typename T>
std::vector<T>* wrap_returned_mpi_alltoall(std::vector<T> data, int sendcount, MPI_Datatype sendtype, int recvcount,
                                           MPI_Datatype recvtype, MPI_Comm comm)
{
  auto new_list = new std::vector<T>();
  new_list->resize(recvcount * comm->size());
  MPI_Alltoall(data.data(), sendcount, sendtype, new_list->data(), recvcount, recvtype, comm);
  return new_list;
}
template <typename T>
void wrap_mpi_alltoall(py::array_t<T> data, int sendcount, MPI_Datatype sendtype, py::array_t<T>& output, int recvcount,
                       MPI_Datatype recvtype, MPI_Comm comm)
{
  py::buffer_info out_buffer = output.request();
  auto* output_ptr           = static_cast<T*>(out_buffer.ptr);
  MPI_Alltoall(data.data(), sendcount, sendtype, output_ptr, recvcount, recvtype, comm);
}
template <typename T> void wrap_mpi_bcast(py::array_t<T> output, int count, MPI_Datatype type, int root, MPI_Comm comm)
{
  py::buffer_info out_buffer = output.request();
  auto* output_ptr           = static_cast<T*>(out_buffer.ptr);
  MPI_Bcast(output_ptr, count, type, root, comm);
}
template <typename T>
void wrap_mpi_reduce_scatter(py::array_t<T> input, py::array_t<T> output, std::vector<int> count, MPI_Datatype type,
                             MPI_Op op, MPI_Comm comm)
{
  py::buffer_info out_buffer = output.request();
  auto* output_ptr           = static_cast<T*>(out_buffer.ptr);
  MPI_Reduce_scatter(input.data(), output_ptr, count.data(), type, op, comm);
}
template <typename T>
void wrap_mpi_reduce_scatter_block(py::array_t<T> input, py::array_t<T> output, int count, MPI_Datatype type, MPI_Op op,
                                   MPI_Comm comm)
{
  py::buffer_info out_buffer = output.request();
  auto* output_ptr           = static_cast<T*>(out_buffer.ptr);
  MPI_Reduce_scatter_block(input.data(), output_ptr, count, type, op, comm);
}
template <typename T>
void wrap_mpi_reduce(py::array_t<T> input, py::array_t<T> output, int count, MPI_Datatype type, MPI_Op op, int root,
                     MPI_Comm comm)
{
  py::buffer_info out_buffer = output.request();
  auto* output_ptr           = static_cast<T*>(out_buffer.ptr);
  MPI_Reduce(input.data(), output_ptr, count, type, op, root, comm);
}
template <typename T>
void wrap_mpi_allreduce(py::array_t<T> input, py::array_t<T> output, int count, MPI_Datatype type, MPI_Op op,
                        MPI_Comm comm)
{
  py::buffer_info out_buffer = output.request();
  auto* output_ptr           = static_cast<T*>(out_buffer.ptr);
  MPI_Allreduce(input.data(), output_ptr, count, type, op, comm);
}
template <typename T>
void wrap_mpi_allgather(py::array_t<T> input, int send_count, MPI_Datatype send_type, py::array_t<T> output,
                        int output_count, MPI_Datatype output_type, MPI_Comm comm)
{
  py::buffer_info out_buffer = output.request();
  auto* output_ptr           = static_cast<T*>(out_buffer.ptr);
  MPI_Allgather(input.data(), send_count, send_type, output_ptr, output_count, output_type, comm);
}
} // namespace

void SMPI_bindings(py::module& m)
{

  static py::object pyForcefulKillEx(py::register_exception<simgrid::ForcefulKillException>(m, "ActorKilled"));

  /* smpi namespace */
  auto smpi_m =
      m.def_submodule("smpi", "Bindings of the s4u::smpi namespace. See the C++ documentation for details.")
          .def("replay_init", &smpi_replay_init, py::call_guard<py::gil_scoped_release>(), "Initialize the SMPI replay")
          .def("replay_run", &smpi_replay_run, py::call_guard<py::gil_scoped_release>(), "Run the SMPI replay")
          .def("replay_main", &smpi_replay_main, py::call_guard<py::gil_scoped_release>(), "Run the SMPI replay main")
          .def("init", &SMPI_init, py::call_guard<py::gil_scoped_release>(), "Initialize the SMPI interface")
          .def("finalize", &SMPI_finalize, py::call_guard<py::gil_scoped_release>(), "Finalize the SMPI interface")
          .def("thread_create", &SMPI_thread_create, py::call_guard<py::gil_scoped_release>(),
               "Initialize the SMPI interface with threads")
          .def(
              "main",
              [](const std::string& name, std::vector<std::string> args) {
                auto argc = static_cast<int>(args.size());
                std::vector<char*> argv(args.size() + 1); // argv[argc] is nullptr
                std::transform(begin(args), end(args), begin(argv), [](std::string& s) { return &s.front(); });
                // Currently this can be dangling, we should wrap this somehow.
                return smpi_main(name.c_str(), argc, argv.data());
              },
              "The constructor should take the parameters from the command line, as is ")
          .def(
              "app_instance_register",
              [](const std::string& name, std::vector<Host*> const hosts, py::object fun, py::args args) {
                fun.inc_ref();  // keep alive after return
                args.inc_ref(); // keep alive after return
                const py::gil_scoped_release gil_release;
                return SMPI_app_instance_start(
                    name.c_str(),
                    [fun_p = fun.ptr(), args_p = args.ptr()]() {
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
                    },
                    hosts);
              },
              "Create an actor from a function or an object. See the :ref:`example <s4u_ex_actors_create>`.")
          .def(
              "app_instance_start",
              [](const std::string& name, py::object fun, std::vector<Host*> const hosts) {
                fun.inc_ref(); // keep alive after return
                const py::gil_scoped_release gil_release;
                return SMPI_app_instance_start(
                    name.c_str(),
                    [fun_p = fun.ptr()]() {
                      const py::gil_scoped_acquire py_context;
                      try {
                        const auto fun = py::reinterpret_borrow<py::object>(fun_p);
                        fun();
                      } catch (const py::error_already_set& ex) {
                        if (ex.matches(pyForcefulKillEx)) {
                          XBT_VERB("Actor killed");
                          simgrid::ForcefulKillException::do_throw(); // Forward that ForcefulKill exception
                        }
                        throw;
                      }
                    },
                    hosts);
              },
              "Create an actor from a function or an object. See the :ref:`example <s4u_ex_actors_create>`.");
  auto mpi_m =
      smpi_m.def_submodule("MPI", "Bindings of MPI functions.")
          .def(
              "Init", []() { return MPI_Init(); }, py::call_guard<py::gil_scoped_release>(),
              "Initialize the MPI interface")
          .def(
              "Init",
              [](std::vector<std::string> args) {
                auto argc = static_cast<int>(args.size());
                std::vector<char*> argv(args.size() + 1); // argv[argc] is nullptr
                std::transform(begin(args), end(args), begin(argv), [](std::string& s) { return &s.front(); });
                // Currently this can be dangling, we should wrap this somehow.
                char** argv_data = argv.data();
                return MPI_Init(&argc, &argv_data);
              },
              "Initialize the MPI interface")
          .def("Finalize", &MPI_Finalize, py::call_guard<py::gil_scoped_release>(), "Finalize the MPI interface");

  py::class_<simgrid::smpi::Info, std::unique_ptr<simgrid::smpi::Info, py::nodelete>> mpi_info(
      mpi_m, "Info", "MPI Info object. See the C++ documentation for details.");
  py::class_<simgrid::smpi::Group, std::unique_ptr<simgrid::smpi::Group, py::nodelete>> mpi_group(
      mpi_m, "Group", "MPI Group object. See the C++ documentation for details.");

  mpi_group
      .def(
          "free", [](simgrid::smpi::Group* group) { return MPI_Group_free(&group); },
          py::call_guard<py::gil_scoped_release>(), "Free the group")
      .def(
          "size",
          [](simgrid::smpi::Group* group) {
            int size;
            MPI_Group_size(group, &size);
            return size;
          },
          py::call_guard<py::gil_scoped_release>(), "Get the group size")
      .def(
          "rank",
          [](simgrid::smpi::Group* group) {
            int rank;
            MPI_Group_rank(group, &rank);
            return rank;
          },
          py::call_guard<py::gil_scoped_release>(), "Get the rank within the group")
      .def(
          "translate_ranks",
          [](simgrid::smpi::Group* group1, std::vector<int> ranks, simgrid::smpi::Group* group2) {
            std::vector<int> ranks2(ranks.size());
            MPI_Group_translate_ranks(group1, ranks.size(), ranks.data(), group2, ranks2.data());
            return ranks2;
          },
          py::call_guard<py::gil_scoped_release>(), "translate ranks from one group to another")
      .def(
          "compare",
          [](simgrid::smpi::Group* group, simgrid::smpi::Group* group2) {
            int result;
            int status = MPI_Group_compare(group, group2, &result);
            return result;
          },
          py::call_guard<py::gil_scoped_release>(), "Compare two groups")
      .def(
          "union",
          [](simgrid::smpi::Group* group, simgrid::smpi::Group* group2) {
            MPI_Group newgroup;
            MPI_Group_union(group, group2, &newgroup);
            return newgroup;
          },
          py::call_guard<py::gil_scoped_release>(), "Return the union of two groups")
      .def(
          "intersection",
          [](simgrid::smpi::Group* group, simgrid::smpi::Group* group2) {
            MPI_Group newgroup;
            MPI_Group_intersection(group, group2, &newgroup);
            return newgroup;
          },
          py::call_guard<py::gil_scoped_release>(), "Return the intersection of two groups")
      .def(
          "difference",
          [](simgrid::smpi::Group* group, simgrid::smpi::Group* group2) {
            MPI_Group newgroup;
            MPI_Group_difference(group, group2, &newgroup);
            return newgroup;
          },
          py::call_guard<py::gil_scoped_release>(), "Return the difference of two groups")
      .def(
          "incl",
          [](simgrid::smpi::Group* group, std::vector<int> ranks) {
            MPI_Group newgroup;
            MPI_Group_incl(group, ranks.size(), ranks.data(), &newgroup);
            return newgroup;
          },
          py::call_guard<py::gil_scoped_release>(), "Return a new group containing the specified ranks")
      .def(
          "excl",
          [](simgrid::smpi::Group* group, std::vector<int> ranks) {
            MPI_Group newgroup;
            MPI_Group_excl(group, ranks.size(), ranks.data(), &newgroup);
            return newgroup;
          },
          py::call_guard<py::gil_scoped_release>(), "Return a new group excluding the specified ranks");

  py::class_<simgrid::smpi::Comm, std::unique_ptr<simgrid::smpi::Comm, py::nodelete>> mpi_comm(
      mpi_m, "Comm", "MPI Communicator object. See the C++ documentation for details.");

  mpi_comm
      .def(
          "size",
          [](simgrid::smpi::Comm* comm) {
            int size;
            MPI_Comm_size(comm, &size);
            return size;
          },
          py::call_guard<py::gil_scoped_release>(), "Get the size of the communicator")
      .def(
          "rank",
          [](simgrid::smpi::Comm* comm) {
            int rank;
            MPI_Comm_rank(comm, &rank);
            return rank;
          },
          py::call_guard<py::gil_scoped_release>(), "Get the rank within the communicator");

  mpi_comm.def(
      "get_name",
      [](simgrid::smpi::Comm* comm) {
        char name[MPI_MAX_OBJECT_NAME];
        int len;
        MPI_Comm_get_name(comm, name, &len);
        return std::string(name);
      },
      py::call_guard<py::gil_scoped_release>(), "Get the name of the communicator");
  mpi_comm.def(
      "set_name", [](simgrid::smpi::Comm* comm, const std::string& name) { MPI_Comm_set_name(comm, name.c_str()); },
      "Set the name of the communicator");
  mpi_comm.def(
      "dup",
      [](simgrid::smpi::Comm* comm) {
        MPI_Comm newcomm;
        MPI_Comm_dup(comm, &newcomm);
        return newcomm;
      },
      py::call_guard<py::gil_scoped_release>(), "Duplicate the communicator");
  mpi_comm.def(
      "dup_with_info",
      [](simgrid::smpi::Comm* comm, MPI_Info info) {
        MPI_Comm newcomm;
        MPI_Comm_dup_with_info(comm, info, &newcomm);
        return newcomm;
      },
      py::call_guard<py::gil_scoped_release>(), "Duplicate the communicator with info");
  mpi_comm.def(
      "group",
      [](simgrid::smpi::Comm* comm) {
        MPI_Group group;
        MPI_Comm_group(comm, &group);
        return group;
      },
      py::call_guard<py::gil_scoped_release>(), "Create a group link to the communicator");
  mpi_comm.def(
      "compare",
      [](simgrid::smpi::Comm* comm1, simgrid::smpi::Comm* comm2) {
        int result;
        MPI_Comm_compare(comm1, comm2, &result);
        return result;
      },
      py::call_guard<py::gil_scoped_release>(), "Compare two communicators");
  mpi_comm.def(
      "create",
      [](simgrid::smpi::Comm* comm, MPI_Group group) {
        MPI_Comm newcomm;
        MPI_Comm_create(comm, group, &newcomm);
        return newcomm;
      },
      py::call_guard<py::gil_scoped_release>(), "Create a new communicator");
  mpi_comm.def(
      "create_group",
      [](simgrid::smpi::Comm* comm, MPI_Group group, int tag) {
        MPI_Comm newcomm;
        MPI_Comm_create_group(comm, group, tag, &newcomm);
        return newcomm;
      },
      py::call_guard<py::gil_scoped_release>(), "Create a new communicator with a tag");
  mpi_comm.def(
      "free", [](simgrid::smpi::Comm* comm) { MPI_Comm_free(&comm); }, "Free the communicator");
  mpi_comm.def(
      "disconnect", [](simgrid::smpi::Comm* comm) { MPI_Comm_disconnect(&comm); }, "Disconnect the communicator");
  mpi_comm.def(
      "split",
      [](simgrid::smpi::Comm* comm, int color, int key) {
        MPI_Comm newcomm;
        MPI_Comm_split(comm, color, key, &newcomm);
        return newcomm;
      },
      py::call_guard<py::gil_scoped_release>(), "Split the communicator");
  mpi_comm.def(
      "set_info", [](simgrid::smpi::Comm* comm, MPI_Info info) { MPI_Comm_set_info(comm, info); },
      "Set the info of the communicator");
  mpi_comm.def(
      "get_info",
      [](simgrid::smpi::Comm* comm) {
        MPI_Info info;
        MPI_Comm_get_info(comm, &info);
        return info;
      },
      py::call_guard<py::gil_scoped_release>(), "Get the info of the communicator");
  mpi_comm.def(
      "split_type",
      [](simgrid::smpi::Comm* comm, int split_type, int key, MPI_Info info) {
        MPI_Comm newcomm;
        MPI_Comm_split_type(comm, split_type, key, info, &newcomm);
        return newcomm;
      },
      py::call_guard<py::gil_scoped_release>(), "Split the communicator by type");
  mpi_comm.def_property_readonly_static("WORLD",
                                        [](py::object /* self */) { return (simgrid::smpi::Comm*)MPI_COMM_WORLD; });
  mpi_comm.def_property_readonly_static("SELF",
                                        [](py::object /* self */) { return (simgrid::smpi::Comm*)MPI_COMM_SELF; });

  BIND_ALL_TYPES(mpi_m, "Alltoall", wrap_mpi_alltoall, py::arg("data"), py::arg("sendcount"), py::arg("sendtype"),
                 py::arg("output"), py::arg("recvcount"), py::arg("recvtype"), py::arg("comm"),
                 py::call_guard<py::gil_scoped_release>(), "Alltoall operation");

  BIND_ALL_TYPES(mpi_m, "Bcast", wrap_mpi_bcast, py::arg("output"), py::arg("count"), py::arg("type"), py::arg("root"),
                 py::arg("comm"), py::call_guard<py::gil_scoped_release>(), "Bcast operation");

  BIND_ALL_TYPES(mpi_m, "Allreduce", wrap_mpi_allreduce, py::arg("input"), py::arg("output"), py::arg("count"),
                 py::arg("type"), py::arg("op"), py::arg("comm"), py::call_guard<py::gil_scoped_release>(),
                 "Allreduce operation");

  BIND_ALL_TYPES(mpi_m, "Allgather", wrap_mpi_allgather, py::arg("input"), py::arg("send_count"), py::arg("send_type"),
                 py::arg("output"), py::arg("output_count"), py::arg("output_type"), py::arg("comm"),
                 py::call_guard<py::gil_scoped_release>(), "Allgather operation");

  BIND_ALL_TYPES(mpi_m, "Reduce", wrap_mpi_reduce, py::arg("input"), py::arg("output"), py::arg("count"),
                 py::arg("type"), py::arg("op"), py::arg("root"), py::arg("comm"),
                 py::call_guard<py::gil_scoped_release>(), "Reduce operation");

  BIND_ALL_TYPES(mpi_m, "Reduce_scatter", wrap_mpi_reduce_scatter, py::arg("input"), py::arg("output"),
                 py::arg("count"), py::arg("type"), py::arg("op"), py::arg("comm"),
                 py::call_guard<py::gil_scoped_release>(), "Reduce scatter operation");

  BIND_ALL_TYPES(mpi_m, "Reduce_scatter_block", wrap_mpi_reduce_scatter_block, py::arg("input"), py::arg("output"),
                 py::arg("count"), py::arg("type"), py::arg("op"), py::arg("comm"),
                 py::call_guard<py::gil_scoped_release>(), "Reduce scatter block operation");

  py::class_<simgrid::smpi::Op, std::unique_ptr<simgrid::smpi::Op, py::nodelete>> mpi_op(
      mpi_m, "Op", "Simulated host. See the C++ documentation for details.");
  mpi_op.def_readonly_static("MAX", MPI_MAX)
      .def_readonly_static("MIN", MPI_MIN)
      .def_readonly_static("MAX", MPI_MAXLOC)
      .def_readonly_static("MIN", MPI_MINLOC)
      .def_readonly_static("SUM", MPI_SUM)
      .def_readonly_static("PROD", MPI_PROD)
      .def_readonly_static("LAND", MPI_LAND)
      .def_readonly_static("LOR", MPI_LOR)
      .def_readonly_static("LXOR", MPI_LXOR)
      .def_readonly_static("BAND", MPI_BAND)
      .def_readonly_static("BOR", MPI_BOR)
      .def_readonly_static("BXOR", MPI_BXOR)
      .def_readonly_static("REPLACE", MPI_REPLACE)
      .def_readonly_static("NO_OP", MPI_NO_OP);

  py::class_<MPI_Status>(mpi_m, "MPI_Status")
      .def(py::init<>())
      .def_readonly("MPI_SOURCE", &MPI_Status::MPI_SOURCE)
      .def_readonly("MPI_TAG", &MPI_Status::MPI_TAG)
      .def_readonly("MPI_ERROR", &MPI_Status::MPI_ERROR)
      .def_readonly("count", &MPI_Status::count)
      .def_readonly("cancelled", &MPI_Status::cancelled);

  py::class_<simgrid::smpi::Datatype, std::unique_ptr<simgrid::smpi::Datatype, py::nodelete>> mpi_datatype(
      mpi_m, "Datatype", "Simulated host. See the C++ documentation for details.");
  mpi_datatype.def_property_readonly_static("DATATYPE_NULL", [](py::object /* self */) { return MPI_DATATYPE_NULL; })
      .def_property_readonly_static("CHAR", [](py::object /* self */) { return MPI_CHAR; })
      .def_property_readonly_static("SHORT", [](py::object /* self */) { return MPI_SHORT; })
      .def_property_readonly_static("INT", [](py::object /* self */) { return MPI_INT; })
      .def_property_readonly_static("LONG", [](py::object /* self */) { return MPI_LONG; })
      .def_property_readonly_static("LONG_LONG", [](py::object /* self */) { return MPI_LONG_LONG; })
      .def_property_readonly_static("SIGNED_CHAR", [](py::object /* self */) { return MPI_SIGNED_CHAR; })
      .def_property_readonly_static("UNSIGNED_CHAR", [](py::object /* self */) { return MPI_UNSIGNED_CHAR; })
      .def_property_readonly_static("UNSIGNED_SHORT", [](py::object /* self */) { return MPI_UNSIGNED_SHORT; })
      .def_property_readonly_static("UNSIGNED", [](py::object /* self */) { return MPI_UNSIGNED; })
      .def_property_readonly_static("UNSIGNED_LONG", [](py::object /* self */) { return MPI_UNSIGNED_LONG; })
      .def_property_readonly_static("UNSIGNED_LONG_LONG", [](py::object /* self */) { return MPI_UNSIGNED_LONG_LONG; })
      .def_property_readonly_static("FLOAT", [](py::object /* self */) { return MPI_FLOAT; })
      .def_property_readonly_static("DOUBLE", [](py::object /* self */) { return MPI_DOUBLE; })
      .def_property_readonly_static("LONG_DOUBLE", [](py::object /* self */) { return MPI_LONG_DOUBLE; })
      .def_property_readonly_static("WCHAR", [](py::object /* self */) { return MPI_WCHAR; })
      .def_property_readonly_static("C_BOOL", [](py::object /* self */) { return MPI_C_BOOL; })
      .def_property_readonly_static("INT8_T", [](py::object /* self */) { return MPI_INT8_T; })
      .def_property_readonly_static("INT16_T", [](py::object /* self */) { return MPI_INT16_T; })
      .def_property_readonly_static("INT32_T", [](py::object /* self */) { return MPI_INT32_T; })
      .def_property_readonly_static("INT64_T", [](py::object /* self */) { return MPI_INT64_T; })
      .def_property_readonly_static("UINT8_T", [](py::object /* self */) { return MPI_UINT8_T; })
      .def_property_readonly_static("BYTE", [](py::object /* self */) { return MPI_BYTE; })
      .def_property_readonly_static("UINT16_T", [](py::object /* self */) { return MPI_UINT16_T; })
      .def_property_readonly_static("UINT32_T", [](py::object /* self */) { return MPI_UINT32_T; })
      .def_property_readonly_static("UINT64_T", [](py::object /* self */) { return MPI_UINT64_T; })
      .def_property_readonly_static("C_FLOAT_COMPLEX", [](py::object /* self */) { return MPI_C_FLOAT_COMPLEX; })
      .def_property_readonly_static("C_DOUBLE_COMPLEX", [](py::object /* self */) { return MPI_C_DOUBLE_COMPLEX; })
      .def_property_readonly_static("C_LONG_DOUBLE_COMPLEX",
                                    [](py::object /* self */) { return MPI_C_LONG_DOUBLE_COMPLEX; })
      .def_property_readonly_static("AINT", [](py::object /* self */) { return MPI_AINT; })
      .def_property_readonly_static("OFFSET", [](py::object /* self */) { return MPI_OFFSET; })
      .def_property_readonly_static("LB", [](py::object /* self */) { return MPI_LB; })
      .def_property_readonly_static("UB", [](py::object /* self */) { return MPI_UB; })
      .def_property_readonly_static("FLOAT_INT", [](py::object /* self */) { return MPI_FLOAT_INT; })
      .def_property_readonly_static("LONG_INT", [](py::object /* self */) { return MPI_LONG_INT; })
      .def_property_readonly_static("DOUBLE_INT", [](py::object /* self */) { return MPI_DOUBLE_INT; })
      .def_property_readonly_static("SHORT_INT", [](py::object /* self */) { return MPI_SHORT_INT; })
      .def_property_readonly_static("2INT", [](py::object /* self */) { return MPI_2INT; })
      .def_property_readonly_static("LONG_DOUBLE_INT", [](py::object /* self */) { return MPI_LONG_DOUBLE_INT; })
      .def_property_readonly_static("2FLOAT", [](py::object /* self */) { return MPI_2FLOAT; })
      .def_property_readonly_static("2DOUBLE", [](py::object /* self */) { return MPI_2DOUBLE; })
      .def_property_readonly_static("2LONG", [](py::object /* self */) { return MPI_2LONG; })
      .def_property_readonly_static("REAL", [](py::object /* self */) { return MPI_REAL; })
      .def_property_readonly_static("REAL4", [](py::object /* self */) { return MPI_REAL4; })
      .def_property_readonly_static("REAL8", [](py::object /* self */) { return MPI_REAL8; })
      .def_property_readonly_static("REAL16", [](py::object /* self */) { return MPI_REAL16; })
      .def_property_readonly_static("COMPLEX8", [](py::object /* self */) { return MPI_COMPLEX8; })
      .def_property_readonly_static("COMPLEX16", [](py::object /* self */) { return MPI_COMPLEX16; })
      .def_property_readonly_static("COMPLEX32", [](py::object /* self */) { return MPI_COMPLEX32; })
      .def_property_readonly_static("INTEGER1", [](py::object /* self */) { return MPI_INTEGER1; })
      .def_property_readonly_static("INTEGER2", [](py::object /* self */) { return MPI_INTEGER2; })
      .def_property_readonly_static("INTEGER4", [](py::object /* self */) { return MPI_INTEGER4; })
      .def_property_readonly_static("INTEGER8", [](py::object /* self */) { return MPI_INTEGER8; })
      .def_property_readonly_static("INTEGER16", [](py::object /* self */) { return MPI_INTEGER16; })
      .def_property_readonly_static("COUNT", [](py::object /* self */) { return MPI_COUNT; });
}