/* Copyright (c) 2008-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/api/states/State.hpp"
#include "src/mc/explo/Exploration.hpp"
#include "src/mc/mc_config.hpp"
#include "xbt/log.h"

#include <algorithm>
#include <boost/range/algorithm.hpp>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_state, mc, "Logging specific to MC states");

namespace simgrid::mc {

long State::expended_states_  = 0;
long State::in_memory_states_ = 0;

State::~State()
{
  in_memory_states_--;
  XBT_VERB("Closing state n°%ld! There are %ld remaining states", this->get_num(), get_in_memory_states());
}

State::State(const RemoteApp& remote_app) : num_(++expended_states_)
{
  in_memory_states_++;
  remote_app.get_actors_status(actors_to_run_);
}

State::State(const RemoteApp& remote_app, StatePtr parent_state) : State(remote_app)
{
  parent_state_        = parent_state;
  incoming_transition_ = parent_state->get_transition_out();
  depth_               = parent_state_->depth_ + 1;

  XBT_DEBUG("Creating %ld, son of %ld", get_num(), parent_state->get_num());
}

std::size_t State::count_todo() const
{
  return boost::range::count_if(actors_to_run_, [](auto& pair) { return pair.second.is_todo(); });
}

bool State::has_more_to_be_explored() const
{
  size_t count = 0;
  for (auto const& [_, actor] : actors_to_run_)
    if (actor.is_todo())
      count += actor.get_times_not_considered();

  return count > 0;
}

aid_t State::next_transition() const
{
  XBT_DEBUG("Search for an actor to run. %zu actors to consider", actors_to_run_.size());
  for (auto const& [aid, actor] : actors_to_run_) {
    /* Only consider actors (1) marked as interleaving by the checker and (2) currently enabled in the application */
    if (not actor.is_todo() || not actor.is_enabled() || actor.is_done()) {
      if (not actor.is_todo())
        XBT_DEBUG("Can't run actor %ld because it is not todo", aid);

      if (not actor.is_enabled())
        XBT_DEBUG("Can't run actor %ld because it is not enabled", aid);

      if (actor.is_done())
        XBT_DEBUG("Can't run actor %ld because it has already been done", aid);

      continue;
    }

    return aid;
  }
  return -1;
}

std::pair<aid_t, int> State::next_transition_guided() const
{
  return Exploration::get_strategy()->next_transition_in(this);
}

// This should be done in GuidedState, or at least interact with it
std::shared_ptr<Transition> State::execute_next(aid_t next, RemoteApp& app)
{
  // This actor is ready to be executed. Execution involves three phases:

  // 1. Identify the appropriate ActorState to prepare for execution
  // when simcall_handle will be called on it
  auto& actor_state               = actors_to_run_.at(next);
  const unsigned times_considered = actor_state.do_consider();
  const Transition* expected_executed_transition = nullptr;
  if (_sg_mc_debug) {
    expected_executed_transition = actor_state.get_transition(times_considered).get();
    xbt_assert(actor_state.is_enabled(), "Tried to execute a disabled actor");
    xbt_assert(expected_executed_transition != nullptr,
               "Expected a transition with %u times considered to be noted in actor %ld", times_considered, next);
  }
  XBT_DEBUG("Let's run actor %ld (times_considered = %u)", next, times_considered);

  // 2. Execute the actor according to the preparation above
  Transition::executed_transitions_++;
  auto* just_executed = app.handle_simcall(next, times_considered, true);
  if (_sg_mc_debug) {
    xbt_assert(
        just_executed->type_ == expected_executed_transition->type_,
        "The transition that was just executed by actor %ld, viz:\n"
        "%s\n"
        "is not what was purportedly scheduled to execute, which was:\n"
        "%s\n"
        "If adding the --cfg=model-check/cached-states-interval:1 parameter solves this problem, then your application "
        "is not purely data dependent (its outcome does not only depends on its input). McSimGrid may miss bugs in "
        "such "
        "applications, as they cannot be exhaustively explored with Mc SimGrid.\n\n"
        "If adding this parameter does not help, then it's probably a bug in Mc SimGrid itself. Please report it.\n",
        next, just_executed->to_string().c_str(), expected_executed_transition->to_string().c_str());
  }
  // 3. Update the state with the newest information. This means recording
  // both
  //  1. what action was last taken from this state (viz. `executed_transition`)
  //  2. what action actor `next` was able to take given `times_considered`
  // The latter update is important as *more* information is potentially available
  // about a transition AFTER it has executed.
  outgoing_transition_ = std::shared_ptr<Transition>(just_executed);

  if (Exploration::get_instance()->need_actor_status_transitions())
    actor_state.set_transition(outgoing_transition_, times_considered);
  app.wait_for_requests();

  return outgoing_transition_;
}

std::unordered_set<aid_t> State::get_backtrack_set() const
{
  std::unordered_set<aid_t> actors;
  for (const auto& [aid, state] : get_actors_list()) {
    if (state.is_todo() || state.is_done()) {
      actors.insert(aid);
    }
  }
  return actors;
}

std::unordered_set<aid_t> State::get_enabled_actors() const
{
  std::unordered_set<aid_t> actors;
  for (const auto& [aid, state] : get_actors_list()) {
    if (state.is_enabled()) {
      actors.insert(aid);
    }
  }
  return actors;
}

std::vector<aid_t> State::get_batrack_minus_done() const
{
  std::vector<aid_t> actors;
  for (const auto& [aid, state] : get_actors_list()) {
    if (state.is_todo()) {
      actors.insert(actors.begin(), aid);
    }
  }
  return actors;
}

void State::register_as_correct()
{
  has_correct_descendent_ = true;
  if (parent_state_ != nullptr)
    parent_state_->register_as_correct();
}

// boost::intrusive_ptr<State> support:
void intrusive_ptr_add_ref(State* state)
{
  state->refcount_.fetch_add(1, std::memory_order_relaxed);
}

void intrusive_ptr_release(State* state)
{
  if (state->refcount_.fetch_sub(1, std::memory_order_release) == 1) {
    std::atomic_thread_fence(std::memory_order_acquire);
    delete state;
  }
}

} // namespace simgrid::mc
