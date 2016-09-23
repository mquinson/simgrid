/* Copyright (c) 2016. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "src/mc/mc_forward.hpp"
#include "src/mc/Checker.hpp"
#include "src/mc/VisitedState.hpp"

#ifndef SIMGRID_MC_COMMUNICATION_DETERMINISM_CHECKER_HPP
#define SIMGRID_MC_COMMUNICATION_DETERMINISM_CHECKER_HPP

namespace simgrid {
namespace mc {

class XBT_PRIVATE CommunicationDeterminismChecker : public Checker {
public:
  CommunicationDeterminismChecker(Session& session);
  ~CommunicationDeterminismChecker();
  int run() override;
  RecordTrace getRecordTrace() override;
  std::vector<std::string> getTextualTrace() override;
private:
  void prepare();
  int main();
  void logState() override;
  void deterministic_comm_pattern(int process, simgrid::mc::PatternCommunication* comm, int backtracking);
  void restoreState();
public:
  // These are used by functions which should be moved in CommunicationDeterminismChecker:
  void get_comm_pattern(xbt_dynar_t list, smx_simcall_t request, e_mc_call_type_t call_type, int backtracking);
  void complete_comm_pattern(
    xbt_dynar_t list, simgrid::mc::RemotePtr<simgrid::kernel::activity::Comm> comm_addr,
    unsigned int issuer, int backtracking);
private:
  /** Stack representing the position in the exploration graph */
  std::list<std::unique_ptr<simgrid::mc::State>> stack_;
  simgrid::mc::VisitedStates visitedStates_;
  unsigned long expandedStatesCount_ = 0;

  int initial_communications_pattern_done = 0;
  int recv_deterministic = 0;
  int send_deterministic = 0;
  char *send_diff = nullptr;
  char *recv_diff = nullptr;
};

#endif

}
}
