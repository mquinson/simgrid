/* Copyright (c) 2008-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/explo/reduction/ODPOR.hpp"
#include "src/mc/api/states/WutState.hpp"
#include "src/mc/explo/Exploration.hpp"
#include "src/mc/explo/reduction/Reduction.hpp"
#include "src/mc/transition/Transition.hpp"
#include "xbt/asserts.h"
#include "xbt/log.h"

#include "src/mc/api/states/SleepSetState.hpp"
#include "src/mc/api/states/State.hpp"
#include <memory>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_odpor, mc_reduction, "Logging specific to the odpor reduction");

namespace simgrid::mc {

std::shared_ptr<Reduction::RaceUpdate> ODPOR::races_computation(odpor::Execution& E, stack_t* S,
                                                                std::vector<StatePtr>* opened_states)
{
  State* s = S->back().get();
  // ODPOR only look for race on the maximal executions
  if (not s->get_enabled_actors().empty())
    return std::make_shared<RaceUpdate>();

  const auto last_event = E.get_latest_event_handle();
  auto updates          = std::make_shared<RaceUpdate>();
  /**
   * ODPOR Race Detection Procedure:
   *
   * For each reversible race in the current execution, we note if there are any continuations `C` equivalent to that
   * which would reverse the race that have already either a) been searched by ODPOR or b) been *noted* to be searched
   * by the wakeup tree at the appropriate reversal point, either as `C` directly or an as equivalent to `C`
   * ("eventually looks like C", viz. the `~_E` relation)
   */
  for (auto e_prime = static_cast<odpor::Execution::EventHandle>(0); e_prime <= last_event.value(); ++e_prime) {
    if (E.get_event_with_handle(e_prime).has_race_been_computed())
      continue;

    XBT_VERB("Computing reversible races of Event `%u`", e_prime);
    for (const auto e : E.get_reversible_races_of(e_prime)) {
      XBT_DEBUG("... racing event `%u``", e);
      WutState* prev_state = static_cast<WutState*>((*S)[e].get());
      xbt_assert(prev_state != nullptr, "ODPOR should use WutState. Fix me");

      if (const auto v = E.get_odpor_extension_from(e, e_prime, *prev_state); v.has_value())
        updates->add_element(prev_state, v.value());
    }
    E.get_event_with_handle(e_prime).consider_races();
  }
  return updates;
}

unsigned long ODPOR::apply_race_update(std::shared_ptr<Reduction::RaceUpdate> updates,
                                       std::vector<StatePtr>* opened_states)
{

  xbt_assert(opened_states == nullptr, "Why is the non BeFS version of ODPOR called with the BeFS variation?");

  unsigned long nb_updates = 0;
  auto odpor_updates = static_cast<RaceUpdate*>(updates.get());

  for (auto& [state, seq] : odpor_updates->get_value()) {
    static_cast<WutState*>(state.get())->insert_into_wakeup_tree(seq);
    nb_updates++;
  }
  return nb_updates;
}

aid_t ODPOR::next_to_explore(odpor::Execution& E, stack_t* S)
{
  auto s = static_cast<WutState*>(S->back().get());
  xbt_assert(s != nullptr, "ODPOR should use WutState. Fix me");
  const aid_t next = s->next_odpor_transition();

  XBT_DEBUG("Asking for the next actor in state #%ld that has the following WuT\n%s", s->get_num(),
            s->string_of_wut().c_str());

  if (next == -1)
    return -1;

  if (not s->is_actor_enabled(next)) {
    XBT_DEBUG("ODPOR wants to execute a disabled transition %s.",
              s->get_actors_list().at(next).get_transition()->to_string(true).c_str());
    s->remove_subtree_at_aid(next);
    s->add_sleep_set(std::make_shared<Transition>(Transition::Type::UNKNOWN, next, 0));
    return next_to_explore(E, S);
  }
  return next;
}
StatePtr ODPOR::state_create(RemoteApp& remote_app, StatePtr parent_state)
{
  if (parent_state == nullptr)
    return StatePtr(new WutState(remote_app), true);
  else
    return StatePtr(new WutState(remote_app, parent_state), true);
}

void ODPOR::on_backtrack(State* s)
{
  StatePtr parent = s->get_parent_state();
  if (parent == nullptr) // this is the root
    return;              // Backtracking from the root means we end exploration, nothing to do

  auto wut_state = static_cast<WutState*>(s);
  wut_state->do_odpor_unwind();
}

void ODPOR::consider_best(StatePtr state)
{
  static_cast<WutState*>(state.get())->add_arbitrary_todo();
}

} // namespace simgrid::mc
