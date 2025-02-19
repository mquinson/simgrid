/* Copyright (c) 2016-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/explo/DFSExplorer.hpp"
#include "simgrid/forward.h"
#include "src/mc/api/states/SoftLockedState.hpp"
#include "src/mc/explo/odpor/Execution.hpp"
#include "src/mc/explo/odpor/odpor_forward.hpp"
#include "src/mc/explo/reduction/BeFSODPOR.hpp"
#include "src/mc/explo/reduction/DPOR.hpp"
#include "src/mc/explo/reduction/NoReduction.hpp"
#include "src/mc/explo/reduction/ODPOR.hpp"
#include "src/mc/explo/reduction/SDPOR.hpp"
#include "src/mc/mc_config.hpp"
#include "src/mc/mc_exit.hpp"
#include "src/mc/mc_forward.hpp"
#include "src/mc/mc_private.hpp"
#include "src/mc/mc_record.hpp"
#include "src/mc/remote/mc_protocol.h"
#include "src/mc/transition/Transition.hpp"

#include "xbt/asserts.h"
#include "xbt/log.h"
#include "xbt/string.hpp"
#include "xbt/sysdep.h"

#include <cassert>
#include <cstdio>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_dfs, mc, "DFS exploration algorithm of the model-checker");

namespace simgrid::mc {

RecordTrace DFSExplorer::get_record_trace() // override
{
  RecordTrace res;

  if (const auto trans = stack_->back()->get_transition_out(); trans != nullptr)
    res.push_back(trans.get());
  for (const auto* state = stack_->back().get(); state != nullptr; state = state->get_parent_state())
    if (state->get_transition_in() != nullptr)
      res.push_front(state->get_transition_in().get());

  return res;
}

void DFSExplorer::log_state() // override
{
  on_log_state_signal(get_remote_app());
  XBT_INFO("DFS exploration ended. %ld unique states visited; %lu explored traces (%lu transition replays, %lu states "
           "visited overall)",
           State::get_expanded_states(), explored_traces_, Transition::get_replayed_transitions(),
           visited_states_count_);
  Exploration::log_state();
}

void DFSExplorer::step_exploration(odpor::Execution& S, aid_t next_actor, stack_t& state_stack)
{

  // This means the exploration asked us to visit a parallel history
  // So first, go to there in the application
  if (not is_execution_descending) {

    backtrack_to_state(state_stack.back().get());
    is_execution_descending = true;
  }

  auto state = state_stack.back();

  // If we use a state containing a sleep state, display it during debug
  if (XBT_LOG_ISENABLED(mc_dfs, xbt_log_priority_verbose) && reduction_mode_ != ReductionMode::none) {
    auto sleep_state = static_cast<SleepSetState*>(state.get());
    if (not sleep_state->get_sleep_set().empty()) {
      XBT_VERB("Sleep set actually containing:");

      for (const auto& [aid, transition] : sleep_state->get_sleep_set())
        XBT_VERB("  <%ld,%s>", aid, transition->to_string().c_str());
    }
  }

  XBT_DEBUG("Going to execute actor %ld", next_actor);

  std::shared_ptr<Transition> executed_transition;
  StatePtr next_state;
  try {
    executed_transition = state->execute_next(next_actor, get_remote_app());
    on_transition_execute_signal(executed_transition.get(), get_remote_app());
    XBT_VERB("Executed %ld: %.60s (stack depth: %zu, state: %ld, %zu interleaves)", executed_transition->aid_,
             executed_transition->to_string().c_str(), state_stack.size(), state->get_num(), state->count_todo());

    next_state = reduction_algo_->state_create(get_remote_app(), state);

    if (_sg_mc_cached_states_interval > 0 && next_state->get_num() % _sg_mc_cached_states_interval == 0) {
      next_state->set_state_factory(get_remote_app().clone_checker_side());
    }
  } catch (McWarning& error) {
    // If an error is reached while executing the transition ...
    if (XBT_LOG_ISENABLED(mc_dfs, xbt_log_priority_debug)) {
      auto transition_to_be_executed = state->get_actors_list().at(next_actor).get_transition();
      XBT_DEBUG("An error occured while executing %ld: %.60s (stack depth: %zu, state: %ld, %zu interleaves)",
                transition_to_be_executed->aid_, transition_to_be_executed->to_string().c_str(), state_stack.size(),
                state->get_num(), state->count_todo());
    }

    // ... reset the application to the step before dying
    // ... the normal backtrack style would be upset, because currently the application is dead
    backtrack_to_state(state.get(), false);

    // ... construct a fake state with no successors so the reduction think it is time to compute races
    next_state = StatePtr(new SoftLockedState(get_remote_app(), state));

    // ... Add that fake state and compute races
    state_stack.emplace_back(std::move(next_state));
    stack_ = &state_stack;
    S.push_transition(std::make_shared<Transition>(Transition::Type::UNKNOWN, next_actor, 0));
    std::shared_ptr<Reduction::RaceUpdate> todo_updates = reduction_algo_->races_computation(S, stack_);
    reduction_algo_->apply_race_update(std::move(todo_updates));

    // ... If we are not already doing it, start critical exploration
    run_critical_exploration_on_need(error.value);

    // ... Unwind the addition of this fake state and prepare to let the algo keep going
    reduction_algo_->on_backtrack(state_stack.back().get());

    is_execution_descending = false;

    state_stack.pop_back();
    stack_ = &state_stack;

    S.remove_last_event();

    // ... reduction trick to keep it sound
    reduction_algo_->consider_best(state);

    return;
  }

  on_state_creation_signal(next_state.get(), get_remote_app());

  state_stack.emplace_back(std::move(next_state));
  stack_ = &state_stack;

  S.push_transition(executed_transition);

  // Backtrack if we reached the maximum depth
  if (state_stack.size() > (std::size_t)_sg_mc_max_depth) {
    if (reduction_mode_ == ReductionMode::dpor) {
      XBT_ERROR("/!\\ Max depth of %d reached! THIS WILL PROBABLY BREAK the dpor reduction /!\\",
                _sg_mc_max_depth.get());
      XBT_ERROR("/!\\ If bad things happen, disable dpor with --cfg=model-check/reduction:none /!\\");
    } else if (reduction_mode_ == ReductionMode::sdpor || reduction_mode_ == ReductionMode::odpor) {
      XBT_ERROR("/!\\ Max depth of %d reached! THIS **WILL** BREAK the reduction, which is not sound "
                "when stopping at a fixed depth /!\\",
                _sg_mc_max_depth.get());
      XBT_ERROR("/!\\ If bad things happen, disable the reduction with --cfg=model-check/reduction:none /!\\");
    } else {
      XBT_WARN("/!\\ Max depth reached ! /!\\ ");
    }
  } else {
    explore(S, state_stack);
  }

  XBT_DEBUG("Backtracking from the exploration by one step");

  reduction_algo_->on_backtrack(state_stack.back().get());

  is_execution_descending = false;

  state_stack.pop_back();
  stack_ = &state_stack;

  S.remove_last_event();
}

void DFSExplorer::explore(odpor::Execution& S, stack_t& state_stack)
{
  visited_states_count_++;

  State* s = state_stack.back().get();

  aid_t next_to_explore;

  while ((next_to_explore = reduction_algo_->next_to_explore(S, &state_stack)) != -1) {

    step_exploration(S, next_to_explore, state_stack);
  }

  std::shared_ptr<Reduction::RaceUpdate> todo_updates = reduction_algo_->races_computation(S, &state_stack);
  reduction_algo_->apply_race_update(std::move(todo_updates));

  XBT_DEBUG("%lu actors remain, but none of them need to be interleaved (depth %zu).", s->get_actor_count(),
            state_stack.size() + 1);
  Exploration::check_deadlock();

  if (s->get_actor_count() == 0) {
    explored_traces_++;
    get_remote_app().finalize_app();
    XBT_VERB("Execution came to an end at %s", get_record_trace().to_string().c_str());
    XBT_VERB("(state: %ld, depth: %zu, %lu explored traces)", s->get_num(), state_stack.size(), backtrack_count_ + 1);
    report_correct_execution(s);
    if (_sg_mc_debug)
      odpor::MazurkiewiczTraces::record_new_execution(S);
  }

  XBT_DEBUG("End of Exploration at depth %lu", S.size() + 1);
}

void DFSExplorer::run()
{
  XBT_INFO("Start a DFS exploration. Reduction is: %s.", to_c_str(reduction_mode_));

  auto initial_state = reduction_algo_->state_create(get_remote_app());
  on_state_creation_signal(initial_state.get(), get_remote_app());

  XBT_DEBUG("**************************************************");

  stack_t state_stack = std::deque<StatePtr>();
  state_stack.emplace_back(std::move(initial_state));

  odpor::Execution empty_seq = odpor::Execution();

  on_exploration_start_signal(get_remote_app());

  explore(empty_seq, state_stack);

  log_state();
}

DFSExplorer::DFSExplorer(std::unique_ptr<RemoteApp> remote_app, ReductionMode mode)
    : Exploration(std::move(remote_app)), reduction_mode_(mode)
{

  if (reduction_mode_ == ReductionMode::dpor)
    reduction_algo_ = std::make_unique<DPOR>();
  else if (reduction_mode_ == ReductionMode::sdpor)
    reduction_algo_ = std::make_unique<SDPOR>();
  else if (reduction_mode_ == ReductionMode::odpor and _sg_mc_explore_algo == "DFS")
    reduction_algo_ = std::make_unique<ODPOR>();
  else if (reduction_mode_ == ReductionMode::odpor and _sg_mc_explore_algo == "BeFS")
    reduction_algo_ = std::make_unique<BeFSODPOR>();
  else {
    xbt_assert(reduction_mode_ == ReductionMode::none, "Reduction mode %s not supported yet by DFS explorer",
               to_c_str(reduction_mode_));
    reduction_algo_ = std::make_unique<NoReduction>();
  }
}

DFSExplorer::DFSExplorer(const std::vector<char*>& args, ReductionMode mode) : Exploration(args), reduction_mode_(mode)
{

  if (reduction_mode_ == ReductionMode::dpor)
    reduction_algo_ = std::make_unique<DPOR>();
  else if (reduction_mode_ == ReductionMode::sdpor)
    reduction_algo_ = std::make_unique<SDPOR>();
  else if (reduction_mode_ == ReductionMode::odpor)
    reduction_algo_ = std::make_unique<ODPOR>();
  else {
    xbt_assert(reduction_mode_ == ReductionMode::none, "Reduction mode %s not supported yet by DFS explorer",
               to_c_str(reduction_mode_));
    reduction_algo_ = std::make_unique<NoReduction>();
  }
}

Exploration* create_dfs_exploration(const std::vector<char*>& args, ReductionMode mode)
{
  return new DFSExplorer(args, mode);
}

} // namespace simgrid::mc
