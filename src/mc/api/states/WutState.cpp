/* Copyright (c) 2007-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/api/states/WutState.hpp"
#include "src/mc/api/RemoteApp.hpp"
#include "src/mc/explo/Exploration.hpp"
#include "src/mc/explo/odpor/WakeupTree.hpp"
#include "src/mc/mc_forward.hpp"
#include "src/mc/transition/Transition.hpp"
#include "xbt/asserts.h"
#include "xbt/log.h"
#include <memory>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_wutstate, mc_state, "States using wakeup tree for ODPOR algorithm");

namespace simgrid::mc {

WutState::WutState(RemoteApp& remote_app) : SleepSetState(remote_app)
{
  initialize_if_empty_wut();
}

WutState::WutState(RemoteApp& remote_app, StatePtr parent_state) : SleepSetState(remote_app, parent_state)
{

  auto parent = static_cast<WutState*>(parent_state.get());

  xbt_assert(parent != nullptr, "Attempting to construct a wakeup tree for the root state "
                                "(or what appears to be, rather for state without a parent defined)");

  XBT_DEBUG("Initializing Wut with parent one:");
  XBT_DEBUG("\n%s", parent->wakeup_tree_.string_of_whole_tree().c_str());

  const auto min_process_node = parent->wakeup_tree_.get_min_single_process_node();
  xbt_assert(min_process_node.has_value(), "Attempting to construct a subtree for a substate from a "
                                           "parent with an empty wakeup tree. This indicates either that ODPOR "
                                           "actor selection in State.cpp is incorrect, or that the code "
                                           "deciding when to make subtrees in ODPOR is incorrect");
  if (not(get_transition_in()->aid_ ==
          min_process_node.value()
              ->get_actor() //&&
                            // get_transition_in()->type_ == min_process_node.value()->get_action()->type_
          )) {
    XBT_ERROR("We tried to make a subtree from a parent state who claimed to have executed `%s` on actor %ld "
              "but whose wakeup tree indicates it should have executed `%s` on actor %ld. This indicates "
              "that exploration is not following ODPOR. Are you sure you're choosing actors "
              "to schedule from the wakeup tree? Trace so far:",
              get_transition_in()->to_string(false).c_str(), get_transition_in()->aid_,
              min_process_node.value()->get_action()->to_string(false).c_str(), min_process_node.value()->get_actor());
    xbt_abort();
  }
  wakeup_tree_ = parent->wakeup_tree_.get_first_subtree();
  initialize_if_empty_wut();
}

aid_t WutState::next_odpor_transition() const
{
  return wakeup_tree_.get_min_single_process_actor().value_or(-1);
}

void WutState::initialize_if_empty_wut()
{
  xbt_assert(!has_initialized_wakeup_tree);
  has_initialized_wakeup_tree = true;

  if (wakeup_tree_.empty())
    add_arbitrary_todo();
}

void WutState::add_arbitrary_todo()
{
  // Find an enabled transition to pick
  auto const [best_actor, _] = Exploration::get_strategy()->best_transition_in(this, false);
  if (best_actor == -1)
    return; // This means that no transitions are enabled at this point
  xbt_assert(sleep_set_.find(best_actor) == sleep_set_.end(),
             "Why is a transition in a sleep set not marked as done? <%ld, %s> is in the sleep set", best_actor,
             sleep_set_.find(best_actor)->second->to_string().c_str());
  consider_one(best_actor);
  auto actor_state = get_actors_list().at(best_actor);
  // For each variant of the transition that is enabled, we want to insert the action into the tree.
  // This ensures that all variants are searched
  for (unsigned times = 0; times < actor_state.get_max_considered(); ++times) {
    // WuT don't really need the transition that will be executed since it will disapear anyway as soon as the child
    // state is created
    wakeup_tree_.insert_at_root(std::make_shared<Transition>(Transition::Type::UNKNOWN, best_actor, times));
  }
}

void WutState::remove_subtree_at_aid(const aid_t proc)
{
  wakeup_tree_.remove_subtree_at_aid(proc);
}

odpor::InsertionResult WutState::insert_into_wakeup_tree(const odpor::PartialExecution& pe)
{
  return this->wakeup_tree_.insert(pe);
}

void WutState::do_odpor_unwind()
{
  XBT_DEBUG("Unwinding ODPOR from state %ld", get_num());
  xbt_assert(get_parent_state() != nullptr, "ODPOR shouldn't try to unwind from root state");

  auto parent = static_cast<WutState*>(get_parent_state());

  // Only when we've exhausted all variants of the transition which
  // can be chosen from this state do we finally add the actor to the
  // sleep set. This ensures that the current logic handling sleep sets
  // works with ODPOR in the way we intend it to work. There is not a
  // good way to perform transition equality in SimGrid; instead, we
  // effectively simply check for the presence of an actor in the sleep set.
  if (not parent->get_actors_list().at(get_transition_in()->aid_).has_more_to_consider()) {
    parent->add_sleep_set((get_transition_in()));
    parent->get_actor_at(get_transition_in()->aid_).mark_done();

    if (parent->wakeup_tree_.get_node_after_actor(get_transition_in()->aid_) != nullptr) {
      xbt_assert(Exploration::get_instance()->is_critical_transition_explorer(),
                 "From my understanding, the only case for the wut to still have something here is that it's the final "
                 "leaf explored before going into critical transition search mode");
      parent->remove_subtree_at_aid(get_transition_in()->aid_);
    }
  }
}

} // namespace simgrid::mc
