/* Copyright (c) 2008-2016. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_SAFETY_CHECKER_HPP
#define SIMGRID_MC_SAFETY_CHECKER_HPP

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "src/mc/mc_forward.hpp"
#include "src/mc/Checker.hpp"
#include "src/mc/VisitedState.hpp"

namespace simgrid {
namespace mc {

class XBT_PRIVATE SafetyChecker : public Checker {
  simgrid::mc::ReductionMode reductionMode_ = simgrid::mc::ReductionMode::unset;
public:
  SafetyChecker(Session& session);
  ~SafetyChecker();
  int run() override;
  RecordTrace getRecordTrace() override;
  std::vector<std::string> getTextualTrace() override;
  void logState() override;
private:
  // Temp
  void init();
  bool checkNonTermination(simgrid::mc::State* current_state);
  int backtrack();
  void restoreState();
private:
  /** Stack representing the position in the exploration graph */
  std::list<std::unique_ptr<simgrid::mc::State>> stack_;
  simgrid::mc::VisitedStates visitedStates_;
  std::unique_ptr<simgrid::mc::VisitedState> visitedState_;
  unsigned long expandedStatesCount_ = 0;
};

}
}

#endif
