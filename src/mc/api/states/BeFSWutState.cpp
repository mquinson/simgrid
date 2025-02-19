/* Copyright (c) 2007-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/api/states/BeFSWutState.hpp"
#include "src/mc/api/RemoteApp.hpp"
#include "src/mc/api/Strategy.hpp"
#include "src/mc/api/states/WutState.hpp"
#include "src/mc/explo/Exploration.hpp"
#include "src/mc/explo/odpor/Execution.hpp"
#include "src/mc/explo/odpor/WakeupTree.hpp"
#include "src/mc/explo/odpor/odpor_forward.hpp"
#include "src/mc/mc_config.hpp"
#include "src/mc/mc_forward.hpp"
#include "src/mc/transition/Transition.hpp"
#include "xbt/asserts.h"
#include "xbt/log.h"
#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_befswutstate, mc_state,
                                "Model-checker state dedicated to the BeFS version of ODPOR algorithm");

namespace simgrid::mc {

BeFSWutState::BeFSWutState(RemoteApp& remote_app) : WutState(remote_app)
{
  is_leftmost_ = true; // The first state is the only one at that depth, so the leftmost one.
}

BeFSWutState::BeFSWutState(RemoteApp& remote_app, StatePtr parent_state) : WutState(remote_app, parent_state)
{
  auto parent = static_cast<BeFSWutState*>(parent_state.get());
  for (const auto& transition : parent->done_) {
    if (not get_transition_in()->depends(transition.get()))
      sleep_add_and_mark(transition);
  }

  is_leftmost_ = parent->is_leftmost_ and parent->done_.size() == parent->closed_.size();
  parent->done_.push_back(get_transition_in());
}

void BeFSWutState::record_child_state(StatePtr child)
{
  aid_t child_aid = outgoing_transition_->aid_;
  if (children_states_.size() < static_cast<long unsigned>(child_aid + 1))
    children_states_.resize(child_aid + 1);
  children_states_[child_aid] = child;
}

std::pair<aid_t, int> BeFSWutState::next_transition_guided() const
{

  aid_t best_actor = this->next_transition();
  if (best_actor == -1)
    return std::make_pair(best_actor, std::numeric_limits<int>::max());
  return std::make_pair(best_actor, Exploration::get_strategy()->get_actor_valuation_in(this, best_actor));
}

aid_t BeFSWutState::next_transition() const
{
  int best_valuation = std::numeric_limits<int>::max();
  int best_actor     = -1;
  for (const aid_t aid : wakeup_tree_.get_direct_children_actors()) {
    if (_sg_mc_debug and
        get_actors_list().find(aid) ==
            get_actors_list().end()) { // the tree is asking to execute someone that doesn't exist in the actor list!
      XBT_CRITICAL("Find a tree pretending that it could execute %ld while this actor is not a possibility (%lu "
                   "existing actors from this state). Find the culprit, and Fix Me. Error @state %ld",
                   aid, get_actors_list().size(), this->get_num());
      XBT_CRITICAL("The existing actors are:");
      for (auto& [aid, astate] : get_actors_list()) {
        XBT_CRITICAL("Actor %ld: %s", aid, astate.get_transition()->to_string().c_str());
      }
      XBT_CRITICAL("While the WuT is:\n%s\n", wakeup_tree_.string_of_whole_tree().c_str());
      xbt_die("Fix me!");
    }
    if (Exploration::get_strategy()->get_actor_valuation_in(this, aid) < best_valuation) {
      best_valuation = Exploration::get_strategy()->get_actor_valuation_in(this, aid);
      best_actor     = aid;
    }
  }
  if (best_actor == -1)
    XBT_DEBUG("Oops, no more people here");
  return best_actor;
}

std::unordered_set<aid_t> BeFSWutState::get_sleeping_actors(aid_t after_actor) const
{
  std::unordered_set<aid_t> actors;
  for (const auto& [aid, _] : get_sleep_set()) {
    actors.insert(aid);
  }
  for (const auto& t : done_) {
    if (t->aid_ == after_actor)
      break;
    actors.insert(t->aid_);
  }
  return actors;
}

StatePtr BeFSWutState::insert_into_final_wakeup_tree(odpor::PartialExecution& w)
{

  // if (sleep_set_.count(w.begin()->get()->aid_ > 0) or
  //     std::find(done_.begin(), done_.end(), w.begin()->get()->aid_) != done_.end())
  //   return nullptr; // The idea here is that the sequence we try to insert has already been covered in some way

  XBT_DEBUG("Inserting at state #%ld sequence\n%s", get_num(), odpor::one_string_textual_trace(w).c_str());
  XBT_DEBUG("... potential children are %s",
            std::accumulate(children_states_.begin(), children_states_.end(), std::string(), [](std::string a, auto b) {
              if (b != nullptr)
                return std::move(a) + ';' + b->get_transition_in()->to_string().c_str();
              else
                return a;
            }).c_str());

  for (auto& t : this->done_) {
    auto aid   = t->aid_;
    auto state = this->children_states_[aid];
    if (state == nullptr)
      continue;

    const auto& next_E_p = state->get_transition_in();

    XBT_DEBUG("... considering state after transition Actor %ld:%s as a candidate", next_E_p->aid_,
              next_E_p->to_string().c_str());

    // Is `p in `I_[E](w)`?
    if (const aid_t p = next_E_p->aid_; odpor::Execution::is_initial_after_execution_of(w, p)) {
      // Remove `p` from w and continue with this node

      // INVARIANT: If `p` occurs in `w`, it had better refer to the same
      // transition referenced by `v`. Unfortunately, we have two
      // sources of truth here which can be manipulated at the same
      // time as arguments to the function. If ODPOR works correctly,
      // they should always refer to the same value; but as a sanity check,
      // we have an assert that tests that at least the types are the same.
      const auto action_by_p_in_w =
          std::find_if(w.begin(), w.end(), [=](const auto& action) { return action->aid_ == p; });
      xbt_assert(action_by_p_in_w != w.end(), "Invariant violated: actor `p` "
                                              "is claimed to be an initial after `w` but is "
                                              "not actually contained in `w`. This indicates that there "
                                              "is a bug computing initials");
      const auto& w_action = *action_by_p_in_w;
      xbt_assert(w_action->type_ == next_E_p->type_,
                 "Invariant violated: `v` claims that actor `%ld` executes '%s' while "
                 "`w` claims that it executes '%s'. These two partial executions both "
                 "refer to `next_[E](p)`, which should be the same",
                 p, next_E_p->to_string(false).c_str(), w_action->to_string(false).c_str());
      w.erase(action_by_p_in_w);
      auto state_befs = static_cast<BeFSWutState*>(state.get());
      return state_befs->insert_into_final_wakeup_tree(w);
    }
    // Is `E ⊢ p ◇ w`?
    else if (odpor::Execution::is_independent_with_execution_of(w, next_E_p)) {
      // Nothing to remove, we simply move on

      // INVARIANT: Note that it is impossible for `p` to be
      // excluded from the set `I_[E](w)` BUT ALSO be contained in
      // `w` itself if `E ⊢ p ◇ w` (intuitively, the fact that `E ⊢ p ◇ w`
      // means that are able to move `p` anywhere in `w` IF it occurred, so
      // if it really does occur we know it must then be an initial).
      // We assert this is the case here
      const auto action_by_p_in_w =
          std::find_if(w.begin(), w.end(), [=](const auto& action) { return action->aid_ == p; });
      xbt_assert(action_by_p_in_w == w.end(),
                 "Invariant violated: We claimed that actor `%ld` is not an initial "
                 "after `w`, yet it's independent with all actions of `w` AND occurs in `w`."
                 "This indicates that there is a bug computing initials",
                 p);
      auto state_befs = static_cast<BeFSWutState*>(state.get());
      return state_befs->insert_into_final_wakeup_tree(w);
    }
  }

  if (w.size() == 0)
    return nullptr;
  insert_into_wakeup_tree(w);
  return this;
}

BeFSWutState::~BeFSWutState()
{
  auto parent_state = get_parent_state();
  if (parent_state != nullptr) {
    auto parent = static_cast<BeFSWutState*>(parent_state);
    parent->closed_.emplace_back(get_transition_in()->aid_);
    parent->signal_on_backtrack();
  }
}

void BeFSWutState::signal_on_backtrack()
{
  XBT_DEBUG("State n°%ld, son of %ld, being signaled to backtrack", get_num(),
            get_parent_state() != nullptr ? get_parent_state()->get_num() : -1);
  XBT_DEBUG("... %s",
            is_leftmost_ ? "This state is the leftmost at this depth" : "This state is NOT the leftmost at this depth");
  XBT_DEBUG("... There are %lu done children, %lu closed children and %lu recorded children StatePtr", done_.size(),
            closed_.size(), children_states_.size());
  if (not is_leftmost_)
    return;

  if (closed_.size() < done_.size()) {
    // if there are children states that are being visited, we may need to update the leftmost information
    aid_t leftmost_aid              = done_[closed_.size()]->aid_;
    auto children_aid               = children_states_[leftmost_aid];
    auto children_befs_aid          = static_cast<BeFSWutState*>(children_aid.get());
    xbt_assert(children_aid != nullptr);
    children_befs_aid->is_leftmost_ = true;
    children_befs_aid->signal_on_backtrack();
    return;
  }

  XBT_DEBUG("... there's still at least one actor to execute (%ld)", next_transition());
  if (next_transition() == -1) {
    // This is the leftmost state, it doesn't have anymore open children and nothing left to do
    // Let's close this by removing it from it's parent
    auto parent_state = get_parent_state();
    if (parent_state != nullptr) {
      auto parent = static_cast<BeFSWutState*>(parent_state);
      XBT_DEBUG("\t... there are %lu recorded children StatePtr in its parent state n°%ld",
                parent->children_states_.size(), parent->get_num());
      parent->children_states_[get_transition_in()->aid_] = nullptr;
    }
  }
}

} // namespace simgrid::mc
