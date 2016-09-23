/* Copyright (c) 2011-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <unistd.h>
#include <sys/wait.h>

#include <memory>

#include <boost/range/algorithm.hpp>

#include <xbt/log.h>
#include <xbt/sysdep.h>

#include "src/mc/mc_comm_pattern.h"
#include "src/mc/mc_safety.h"
#include "src/mc/mc_private.h"
#include "src/mc/Process.hpp"
#include "src/mc/mc_smx.h"
#include "src/mc/VisitedState.hpp"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_VisitedState, mc,
                                "Logging specific to state equaity detection mechanisms");

namespace simgrid {
namespace mc {

static int snapshot_compare(simgrid::mc::VisitedState* state1, simgrid::mc::VisitedState* state2)
{
  simgrid::mc::Snapshot* s1 = state1->system_state.get();
  simgrid::mc::Snapshot* s2 = state2->system_state.get();
  int num1 = state1->num;
  int num2 = state2->num;
  return snapshot_compare(num1, s1, num2, s2);
}

/**
 * \brief Save the current state
 * \return Snapshot of the current state.
 */
VisitedState::VisitedState(unsigned long state_number)
{
  simgrid::mc::Process* process = &(mc_model_checker->process());
  this->heap_bytes_used = mmalloc_get_bytes_used_remote(
    process->get_heap()->heaplimit,
    process->get_malloc_info());

  this->nb_processes =
    mc_model_checker->process().simix_processes().size();

  this->system_state = simgrid::mc::take_snapshot(state_number);
  this->num = state_number;
  this->other_num = -1;
}

VisitedState::~VisitedState()
{
}

void VisitedStates::prune()
{
  while (states_.size() > (std::size_t) _sg_mc_visited) {
    XBT_DEBUG("Try to remove visited state (maximum number of stored states reached)");
    auto min_element = boost::range::min_element(states_,
      [](std::unique_ptr<simgrid::mc::VisitedState>& a, std::unique_ptr<simgrid::mc::VisitedState>& b) {
        return a->num < b->num;
      });
    xbt_assert(min_element != states_.end());
    // and drop it:
    states_.erase(min_element);
    XBT_DEBUG("Remove visited state (maximum number of stored states reached)");
  }
}

/**
 * \brief Checks whether a given state has already been visited by the algorithm.
 */
std::unique_ptr<simgrid::mc::VisitedState> VisitedStates::addVisitedState(
  unsigned long state_number, simgrid::mc::State* graph_state, bool compare_snpashots)
{
  std::unique_ptr<simgrid::mc::VisitedState> new_state =
    std::unique_ptr<simgrid::mc::VisitedState>(new VisitedState(state_number));
  graph_state->system_state = new_state->system_state;
  XBT_DEBUG("Snapshot %p of visited state %d (exploration stack state %d)",
    new_state->system_state.get(), new_state->num, graph_state->num);

  auto range = boost::range::equal_range(states_,
    new_state.get(), simgrid::mc::DerefAndCompareByNbProcessesAndUsedHeap());

  if (compare_snpashots)
    for (auto i = range.first; i != range.second; ++i) {
      auto& visited_state = *i;
      if (snapshot_compare(visited_state.get(), new_state.get()) == 0) {
        // The state has been visited:

        std::unique_ptr<simgrid::mc::VisitedState> old_state =
          std::move(visited_state);

        if (old_state->other_num == -1)
          new_state->other_num = old_state->num;
        else
          new_state->other_num = old_state->other_num;

        if (dot_output == nullptr)
          XBT_DEBUG("State %d already visited ! (equal to state %d)",
            new_state->num, old_state->num);
        else
          XBT_DEBUG(
            "State %d already visited ! (equal to state %d (state %d in dot_output))",
            new_state->num, old_state->num, new_state->other_num);

        /* Replace the old state with the new one (with a bigger num)
           (when the max number of visited states is reached,  the oldest
           one is removed according to its number (= with the min number) */
        XBT_DEBUG("Replace visited state %d with the new visited state %d",
          old_state->num, new_state->num);

        visited_state = std::move(new_state);
        return std::move(old_state);
      }
    }

  XBT_DEBUG("Insert new visited state %d (total : %lu)", new_state->num, (unsigned long) states_.size());
  states_.insert(range.first, std::move(new_state));
  this->prune();
  return nullptr;
}

}
}