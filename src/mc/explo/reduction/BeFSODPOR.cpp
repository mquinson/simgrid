/* Copyright (c) 2008-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/explo/reduction/BeFSODPOR.hpp"
#include "src/mc/api/states/BeFSWutState.hpp"
#include "src/mc/api/states/WutState.hpp"
#include "src/mc/explo/Exploration.hpp"
#include "src/mc/explo/odpor/Execution.hpp"

#include "src/mc/mc_forward.hpp"
#include "xbt/asserts.h"
#include "xbt/log.h"

#include "src/mc/api/states/SleepSetState.hpp"
#include "src/mc/api/states/State.hpp"
#include <string>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_befsodpor, mc_reduction, "Logging specific to the BeFS ODPOR reduction");

namespace simgrid::mc {

std::shared_ptr<Reduction::RaceUpdate> BeFSODPOR::races_computation(odpor::Execution& E, stack_t* S,
                                                                    std::vector<StatePtr>* opened_states)
{
  if (opened_states == nullptr)
    XBT_VERB("calling BeFSODPOR outside of BeFS algorithm: the only case this should happen is if you are looking for "
             "the critical transition");

  State* s = S->back().get();
  // ODPOR only look for race on the maximal executions
  if (not s->get_enabled_actors().empty()) {
    return std::make_shared<RaceUpdate>();
  }

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
  XBT_DEBUG("Going to compute all the reversible races on sequence \n%s", E.get_one_string_textual_trace().c_str());
  for (auto e_prime = static_cast<odpor::Execution::EventHandle>(0); e_prime <= last_event.value(); ++e_prime) {
    if (E.get_event_with_handle(e_prime).has_race_been_computed())
      continue;
    XBT_VERB("Computing reversible races of Event `%u`", e_prime);
    for (const auto e : E.get_reversible_races_of(e_prime)) {
      XBT_DEBUG("... racing event `%u`", e);
      XBT_DEBUG("... race between event `%u`:%s and event `%u`:%s", e_prime,
                E.get_transition_for_handle(e_prime)->to_string().c_str(), e,
                E.get_transition_for_handle(e)->to_string().c_str());
      BeFSWutState* prev_state = static_cast<BeFSWutState*>((*S)[e].get());
      xbt_assert(prev_state != nullptr, "ODPOR should use WutState. Fix me");

      auto actor_after_e = ((*S)[e + 1].get())->get_transition_in()->aid_;

      if (const auto v = E.get_odpor_extension_from(e, e_prime, *prev_state, actor_after_e); v.has_value()) {
        XBT_DEBUG("... inserting sequence %s in final_wut before event `%u`",
                  odpor::one_string_textual_trace(v.value()).c_str(), e);
        updates->add_element(prev_state, v.value());
      }
    }
    E.get_event_with_handle(e_prime).consider_races();
  }
  return updates;
}

unsigned long BeFSODPOR::apply_race_update(std::shared_ptr<Reduction::RaceUpdate> updates,
                                           std::vector<StatePtr>* opened_states)
{
  if (opened_states == nullptr)
    XBT_VERB("calling BeFS ODPOR outside of BeFS algorithm: the only case this should happen is if you are looking for "
             "the critical transition");

  auto befsodpor_updates   = static_cast<RaceUpdate*>(updates.get());
  unsigned long nb_updates = 0;
  for (auto& [raw_state, v] : befsodpor_updates->get_value()) {
    XBT_DEBUG("Going to insert sequence\n%s", odpor::one_string_textual_trace(v).c_str());
    XBT_DEBUG("... at state #%ld", raw_state->get_num());
    auto state         = static_cast<BeFSWutState*>(raw_state.get());
    const auto inserted_state = state->insert_into_final_wakeup_tree(v);
    if (opened_states != nullptr and inserted_state != nullptr) {
      opened_states->push_back(inserted_state);
      XBT_DEBUG("... ended up adding work to do at state #%ld", inserted_state->get_num());
      XBT_DEBUG("... WuT at that state after insertion\n%s",
                static_cast<WutState*>(inserted_state.get())->string_of_wut().c_str());
    }
    nb_updates++;
  }
  return nb_updates;
}

aid_t BeFSODPOR::next_to_explore(odpor::Execution& E, stack_t* S)
{
  auto s = static_cast<BeFSWutState*>(S->back().get());
  xbt_assert(s != nullptr, "BeFS ODPOR should use BeFSWutState. Fix me");
  const aid_t next = s->next_transition();

  if (next == -1)
    return -1;

  XBT_DEBUG("Picking actor %ld among this WuT: %s", next, s->string_of_wut().c_str());

  if (not s->is_actor_enabled(next)) {
    XBT_DEBUG("BeFS ODPOR wants to execute a disabled transition %s.",
              s->get_actors_list().at(next).get_transition()->to_string(true).c_str());
    s->remove_subtree_at_aid(next);
    s->add_sleep_set(std::make_shared<Transition>(Transition::Type::UNKNOWN, next, 0));
    return next_to_explore(E, S);
  }

  return next;
}

StatePtr BeFSODPOR::state_create(RemoteApp& remote_app, StatePtr parent_state)
{
  if (parent_state == nullptr)
    return StatePtr(new BeFSWutState(remote_app), true);
  else {
    BeFSWutState* befswut_state = static_cast<BeFSWutState*>(parent_state.get());
    if (auto existing_state = befswut_state->get_children_state_of_aid(parent_state->get_transition_out()->aid_);
        existing_state != nullptr) {
      return existing_state;
    }
    auto new_state = StatePtr(new BeFSWutState(remote_app, befswut_state), true);
    befswut_state->record_child_state(new_state);
    return new_state;
  }
}

} // namespace simgrid::mc
