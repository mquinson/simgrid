/* Copyright (c) 2007-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_BeFSWUTSTATE_HPP
#define SIMGRID_MC_BeFSWUTSTATE_HPP

#include "src/mc/api/RemoteApp.hpp"
#include "src/mc/api/states/WutState.hpp"
#include "src/mc/explo/odpor/WakeupTree.hpp"
#include "src/mc/explo/odpor/odpor_forward.hpp"
#include "src/mc/mc_forward.hpp"
#include <unordered_set>
#include <vector>

namespace simgrid::mc {

class XBT_PRIVATE BeFSWutState : public WutState {
  std::vector<StatePtr> children_states_; // Key is aid

  /** Store the aid that have been visited at least once. This is usefull both to know what not to
   *  revisit, but also to remember the order in which the children were visited. The latter information
   *  being important for the correction. */
  std::vector<std::shared_ptr<Transition>> done_;

  /** Only leftmosts states of the tree can be closed. This is decided on creation based on parent
   *  value, and then updated when nearby states are closed. */
  bool is_leftmost_;

  /** Store the aid that have been closed. This is usefull to determine wether a given state is leftmost. */
  std::vector<aid_t> closed_;

public:
  explicit BeFSWutState(RemoteApp& remote_app);
  explicit BeFSWutState(RemoteApp& remote_app, StatePtr parent_state);
  ~BeFSWutState();

  void record_child_state(StatePtr child);

  aid_t next_transition() const;

  /** Insert the sequence given inside the current exploration tree. The name is given in reference to the
   *  article (yet to be published). The idea of the implementation is to consider the existing tree-like structure
   *  of the state as the final wakeuptree and recursively insert the sequence at the right place. When we cannot
   *  recursively visit any children, insert the remaining stuff in the WuT of the obtained state, and return that
   *  state.*/
  StatePtr insert_into_final_wakeup_tree(odpor::PartialExecution&);

  std::pair<aid_t, int> next_transition_guided() const override;

  StatePtr get_children_state_of_aid(aid_t next)
  {
    if (next >= 0 && children_states_.size() > static_cast<long unsigned>(next))
      return children_states_[next];
    return nullptr;
  }

  /**
   * @brief Take the Wakeup tree from the parent and extract the correponding subtree in this state.
   */
  void unwind_wakeup_tree_from_parent();

  std::unordered_set<aid_t> get_sleeping_actors(aid_t after_actor) const override;

  /**
   * @brief Recursively unroll the given sequence into childs until none corresponding is find. The function
   * then insert the remaining subsequence and return the corresponding state
   */
  StatePtr force_insert_into_wakeup_tree(const odpor::PartialExecution&);

  void compare_final_and_wut();

  /**
   * @brief Called when this state is backtracked in the exploration. Since this state is used in best first search,
   * the signal is also called by other states being backtracked, making sure every state can be closed accordingly.
   */
  void signal_on_backtrack() override;
};

} // namespace simgrid::mc

#endif
