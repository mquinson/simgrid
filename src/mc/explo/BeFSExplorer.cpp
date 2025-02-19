/* Copyright (c) 2016-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/explo/BeFSExplorer.hpp"
#include "src/mc/api/RemoteApp.hpp"
#include "src/mc/explo/odpor/Execution.hpp"
#include "src/mc/mc_config.hpp"
#include "src/mc/mc_exit.hpp"
#include "src/mc/mc_forward.hpp"
#include "src/mc/mc_private.hpp"
#include "src/mc/mc_record.hpp"
#include "src/mc/remote/CheckerSide.hpp"
#include "src/mc/remote/mc_protocol.h"
#include "src/mc/transition/Transition.hpp"

#include "src/mc/explo/reduction/BeFSODPOR.hpp"
#include "src/mc/explo/reduction/DPOR.hpp"
#include "src/mc/explo/reduction/NoReduction.hpp"
#include "src/mc/explo/reduction/Reduction.hpp"
#include "src/mc/explo/reduction/SDPOR.hpp"

#include "xbt/asserts.h"
#include "xbt/log.h"
#include "xbt/random.hpp"
#include "xbt/string.hpp"
#include "xbt/sysdep.h"

#include <cassert>
#include <climits>
#include <cstdio>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_befs, mc, "BeFS exploration algorithm of the model-checker");

namespace simgrid::mc {

RecordTrace BeFSExplorer::get_record_trace() // override
{
  RecordTrace res;

  if (const auto trans = stack_.back()->get_transition_out(); trans != nullptr)
    res.push_back(trans.get());
  for (const auto* state = stack_.back().get(); state != nullptr; state = state->get_parent_state())
    if (state->get_transition_in() != nullptr)
      res.push_front(state->get_transition_in().get());

  return res;
}

void BeFSExplorer::restore_stack(StatePtr state)
{
  XBT_DEBUG("Going to restore stack. Current depth is %lu; chosen state is #%ld", stack_.size(), state->get_num());
  stack_.clear();
  execution_seq_     = odpor::Execution();
  auto current_state = state;
  stack_.emplace_front(current_state);
  // condition corresponds to reaching initial state
  while (current_state->get_parent_state() != nullptr) {
    current_state = current_state->get_parent_state();
    stack_.emplace_front(current_state);
  }
  XBT_DEBUG("Replaced stack by %s", get_record_trace().to_string().c_str());
  // NOTE: The outgoing transition for the top-most state of the  stack refers to that which was taken
  // as part of the last trace explored by the algorithm. Thus, only the sequence of transitions leading up to,
  // but not including, the last state must be included when reconstructing the Exploration for SDPOR.
  for (auto iter = std::next(stack_.begin()); iter != stack_.end(); ++iter) {
    // if we are exploring in a branch BeFS manner (and not a step manner), it is safe to assume that the
    // races for the state we restore have already been computed once
    execution_seq_.push_transition((*iter)->get_transition_in(), _sg_mc_befs_threshold == 0);
  }
}

void BeFSExplorer::log_state() // override
{
  on_log_state_signal(get_remote_app());
  XBT_INFO("BeFS exploration ended. %ld unique states visited; %lu explored traces (%lu transition replays, "
           "%lu states "
           "visited overall)",
           State::get_expanded_states(), explored_traces_, Transition::get_replayed_transitions(),
           visited_states_count_);
  Exploration::log_state();
}

void BeFSExplorer::run()
{
  on_exploration_start_signal(get_remote_app());
  /* This function runs the Best First Search algorithm the state space.
   * We do so iteratively instead of recursively, dealing with the call stack manually.
   * This allows one to explore the call stack at will. */

  while (not stack_.empty()) {
    /* Get current state */
    auto state = stack_.back();

    XBT_DEBUG("**************************************************");
    XBT_DEBUG("Exploration depth=%zu (state:#%ld; %zu interleaves todo; %lu currently opened states)", stack_.size(),
              state->get_num(), state->count_todo(), opened_states_.size());

    // Backtrack if we reached the maximum depth
    if (stack_.size() > (std::size_t)_sg_mc_max_depth) {
      XBT_WARN("/!\\ Max depth of %d reached! THIS WILL PROBABLY BREAK the reduction /!\\", _sg_mc_max_depth.get());
      XBT_WARN("/!\\ Any bug you may find are real, but not finding bug doesn't mean anything /!\\");
      XBT_WARN("/!\\ You should consider changing the depth limit with --cfg=model-check/max-depth /!\\");
      XBT_WARN("/!\\ Asumming you know what you are doing, the programm will now backtrack /!\\");
      this->backtrack();
      continue;
    }

    // Search for the next transition
    // next_transition returns a pair<aid_t, int>
    // in case we want to consider multiple states (eg. during backtrack)
    const aid_t next = reduction_algo_->next_to_explore(execution_seq_, &stack_);

    if (next < 0) {

      // If there is no more transition in the current state (or if ODPOR picked an actor that is not enabled --
      // ReversibleRace is an overapproximation), backtrace
      XBT_VERB("%lu actors remain, but none of them need to be interleaved (depth %zu).", state->get_actor_count(),
               stack_.size() + 1);
      Exploration::check_deadlock();

      if (state->get_actor_count() == 0) {
        // Compute the race when reaching a leaf, and apply them immediately
        std::shared_ptr<Reduction::RaceUpdate> todo_updates =
            reduction_algo_->races_computation(execution_seq_, &stack_, &opened_states_);
        reduction_algo_->apply_race_update(std::move(todo_updates), &opened_states_);

        explored_traces_++;
        // Costly verification used to check against algorithm optimality
        if (_sg_mc_debug)
          odpor::MazurkiewiczTraces::record_new_execution(execution_seq_);
        get_remote_app().finalize_app();
        XBT_VERB("Execution came to an end at %s", get_record_trace().to_string().c_str());
        XBT_VERB("(state: %ld, depth: %zu, %lu explored traces)", state->get_num(), stack_.size(), explored_traces_);
        report_correct_execution(state.get());
      }

      this->backtrack();
      continue;
    }

    xbt_assert(state->is_actor_enabled(next));

    if (_sg_mc_befs_threshold != 0) {
      auto dist = Exploration::get_strategy()->get_actor_valuation_in(state.get(), next);
      auto best = best_opened_state();
      if (best != nullptr) {
        int best_dist = best->next_transition_guided().second;
        opened_states_.emplace_back(std::move(best));
        if ((float)(dist * _sg_mc_befs_threshold) / 100.0 > (float)best_dist) {
          XBT_VERB("current selected dist:%d vs. best*rate:%d", dist, best_dist);
          opened_states_.emplace_back(std::move(state));
          this->backtrack();
          continue;
        }
      }
    }

    // If we use a state containing a sleep state, display it during debug
    if (XBT_LOG_ISENABLED(mc_befs, xbt_log_priority_verbose) && reduction_mode_ != ReductionMode::none) {
      auto sleep_state = static_cast<SleepSetState*>(state.get());
      if (not sleep_state->get_sleep_set().empty()) {
        XBT_VERB("Sleep set actually containing:");

        for (const auto& [aid, transition] : sleep_state->get_sleep_set())
          XBT_VERB("  <%ld,%s>", aid, transition->to_string().c_str());
      }
    }

    if (XBT_LOG_ISENABLED(mc_befs, xbt_log_priority_debug)) {
      auto todo = state->get_actors_list().at(next).get_transition();
      XBT_DEBUG("wanna execute %ld: %.60s", next, todo->to_string().c_str());
    }

    /* Actually answer the request: let's execute the selected request (MCed does one step) */
    auto executed_transition = state->execute_next(next, get_remote_app());
    on_transition_execute_signal(state->get_transition_out().get(), get_remote_app());

    // If there are processes to interleave and the maximum depth has not been
    // reached then perform one step of the exploration algorithm.
    XBT_VERB("Executed %ld: %.60s (stack depth: %zu, state: %ld, %zu interleaves, %lu opened states)",
             state->get_transition_out()->aid_, state->get_transition_out()->to_string().c_str(), stack_.size(),
             state->get_num(), state->count_todo(), opened_states_.size());

    /* Create the new expanded state (copy the state of MCed into our MCer data) */
    auto next_state = reduction_algo_->state_create(get_remote_app(), state);

    if (_sg_mc_cached_states_interval > 0 && next_state->get_num() % _sg_mc_cached_states_interval == 0) {
      static unsigned max_files = sysconf(_SC_OPEN_MAX);

      // Save 100 FDs for when we want to restart an old fork: we need a new socket for it
      if (CheckerSide::get_count() > max_files - 100) {
        // For now, each CheckerSide takes 4 FDs, and we have about 12 FDs before creating the first CheckerSide
        int cur_files =
            std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{});
        XBT_CRITICAL("Skipping a cached state because the amount of open files is too high: %d open files out of %u. "
                     "Please increase the max with `ulimit -n <value>` to improve the performances.",
                     (int)cur_files, max_files);
      } else
        next_state->set_state_factory(get_remote_app().clone_checker_side());
    }
    on_state_creation_signal(next_state.get(), get_remote_app());

    visited_states_count_++;

    reduction_algo_->on_backtrack(state.get());

    // Before leaving that state, if the transition we just took can be taken multiple times, we
    // need to give it to the opened states
    if (stack_.back()->has_more_to_be_explored() > 0)
      opened_states_.emplace_back(state);

    stack_.emplace_back(std::move(next_state));
    execution_seq_.push_transition(std::move(executed_transition));

    if (dot_output_ != nullptr)
      dot_output("\"%ld\" -> \"%ld\" [%s];\n", state->get_num(), stack_.back()->get_num(),
                 state->get_transition_out()->dot_string().c_str());
  }
  log_state();
}

StatePtr BeFSExplorer::best_opened_state()
{
  // If we work in a DFS like manner, just forget about the opened_states
  if (_sg_mc_strategy == "none") {
    opened_states_.clear();
    auto candidate = stack_.back();
    while (candidate->next_transition_guided().first == -1) {
      if (candidate->get_parent_state() != nullptr)
        candidate = candidate->get_parent_state();
      else
        return nullptr; // There is no one with something to do left
    }
    return candidate;

    // Else we are picking uniformly among opened_states
  } else {
    if (opened_states_.size() == 0)
      return nullptr;
    int guess          = xbt::random::uniform_int(0, opened_states_.size() - 1);
    StatePtr candidate = opened_states_[guess];
    opened_states_.erase(opened_states_.begin() + guess);
    while (candidate->next_transition_guided().first == -1) {
      if (opened_states_.size() == 0)
        return nullptr;
      guess     = xbt::random::uniform_int(0, opened_states_.size() - 1);
      candidate = opened_states_[guess];
      opened_states_.erase(opened_states_.begin() + guess);
    }
    return candidate;
  }
}

void BeFSExplorer::backtrack()
{
  XBT_VERB("Backtracking from %s", get_record_trace().to_string().c_str());
  XBT_DEBUG("%lu alternatives are yet to be explored:", opened_states_.size());

  Exploration::check_deadlock();

  stack_.back()->signal_on_backtrack();

  // Take the point with smallest distance
  auto backtracking_point = best_opened_state();

  // if no backtracking point, then set the stack_ to empty so we can end the exploration
  if (not backtracking_point) {
    XBT_DEBUG("No more opened point of exploration, the search will end");
    stack_.clear();
    return;
  }
  // We found a backtracking point, let's go to it
  backtrack_to_state(backtracking_point.get());
  this->restore_stack(backtracking_point);
}

BeFSExplorer::BeFSExplorer(const std::vector<char*>& args, ReductionMode mode)
    : Exploration(args), reduction_mode_(mode)
{

  if (reduction_mode_ == ReductionMode::dpor)
    reduction_algo_ = std::make_unique<DPOR>();
  else if (reduction_mode_ == ReductionMode::sdpor)
    reduction_algo_ = std::make_unique<SDPOR>();
  else if (reduction_mode_ == ReductionMode::odpor)
    reduction_algo_ = std::make_unique<BeFSODPOR>();
  else {
    xbt_assert(reduction_mode_ == ReductionMode::none, "Reduction mode %s not supported yet by BeFS explorer",
               to_c_str(reduction_mode_));
    reduction_algo_ = std::make_unique<NoReduction>();
  }

  XBT_INFO("Start a BeFS exploration. Reduction is: %s.", to_c_str(reduction_mode_));

  auto initial_state = reduction_algo_->state_create(get_remote_app());

  XBT_DEBUG("**************************************************");

  stack_.emplace_back(std::move(initial_state));
  visited_states_count_++;

  /* Get an enabled actor and insert it in the interleave set of the initial state */
  XBT_DEBUG("Initial state. %lu actors to consider", stack_.back()->get_actor_count());

  opened_states_.emplace_back(stack_.back());
}

Exploration* create_befs_exploration(const std::vector<char*>& args, ReductionMode mode)
{
  return new BeFSExplorer(args, mode);
}

} // namespace simgrid::mc
