/* Copyright (c) 2007-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_WUTSTATE_HPP
#define SIMGRID_MC_WUTSTATE_HPP

#include "src/mc/api/RemoteApp.hpp"
#include "src/mc/api/states/SleepSetState.hpp"
#include "src/mc/explo/odpor/WakeupTree.hpp"
#include <memory>

namespace simgrid::mc {

class XBT_PRIVATE WutState : public SleepSetState {

  bool has_initialized_wakeup_tree = false;

protected:
  /**
   * The wakeup tree with respect to the execution represented
   * by the totality of all states before and including this one
   * and with respect to this state's sleep set
   */
  odpor::WakeupTree wakeup_tree_;

  void initialize_if_empty_wut();

public:
  explicit WutState(RemoteApp& remote_app);
  explicit WutState(RemoteApp& remote_app, StatePtr parent_state);

  /**
   * Same as next_transition(), but the choice is based off the ODPOR
   * wakeup tree associated with this state
   */
  aid_t next_odpor_transition() const;

  /**
   * @brief Removes the subtree rooted at the single-process node
   * `N` running actor `p` of this state's wakeup tree
   */
  void remove_subtree_using_children_in_transition(const std::shared_ptr<Transition> transition);
  void remove_subtree_at_aid(aid_t proc);
  bool has_empty_tree() const { return this->wakeup_tree_.empty(); }
  std::string string_of_wut() const { return this->wakeup_tree_.string_of_whole_tree(); }

  /**
   * @brief
   */
  odpor::InsertionResult insert_into_wakeup_tree(const odpor::PartialExecution&);

  /** @brief Prepares the parent state for re-exploration following
   * another after having followed ODPOR from this state with
   * the current in transition
   *
   * After ODPOR has completed searching a maximal trace, it
   * finds the first point in the execution with a nonempty wakeup
   * tree. This method corresponds to lines 20 and 21 in the ODPOR
   * pseudocode
   */
  void do_odpor_unwind();

  unsigned int direct_children() const { return wakeup_tree_.count_direct_children(); }

  bool has_more_to_be_explored() const override { return direct_children() > 0; }

  /**
   * @brief Adds an arbitrary enabled transition to the wakeup tree.
   *
   * 99 percent of the times, this is not the way you want to do this. Instead
   * use other methods that already handle the wakeup tree by themselves. If
   * you think you are in the other 1 percent, write a paper about your new
   * algorithm first, and only then use this method.
   */
  void add_arbitrary_todo();
};

} // namespace simgrid::mc

#endif
