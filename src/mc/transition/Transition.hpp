/* Copyright (c) 2015-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_TRANSITION_HPP
#define SIMGRID_MC_TRANSITION_HPP

#include "simgrid/forward.h" // aid_t
#include "xbt/ex.h"
#include "xbt/utility.hpp"   // XBT_DECLARE_ENUM_CLASS

#include <sstream>
#include <string>

namespace simgrid::mc {

/** An element in the recorded path
 *
 *  At each decision point, we need to record which process transition
 *  is triggered and potentially which value is associated with this
 *  transition. The value is used to find which communication is triggered
 *  in things like waitany and for associating a given value of MC_random()
 *  calls.
 */
class Transition {
  /* Global statistics */
  static unsigned long executed_transitions_;

  friend State; // FIXME remove this once we have a proper class to handle the statistics

public:
  static unsigned long replayed_transitions_;

  /* Ordering is important here. depends() implementations only consider subsequent types in this ordering */
  XBT_DECLARE_ENUM_CLASS(
      Type,
      /* First because indep with anybody including themselves */
      RANDOM, ACTOR_JOIN, ACTOR_SLEEP,
      /* high priority because indep with everybody but the local actions of the child */
      ACTOR_CREATE,
      /* high priority because indep with almost everybody */
      OBJECT_ACCESS,
      /* high priority because they can rewrite themselves to *_WAIT */
      TESTANY, WAITANY,
      /* BARRIER transitions sorted alphabetically */
      BARRIER_ASYNC_LOCK, BARRIER_WAIT,
      /* Alphabetical ordering of COMM_* */
      COMM_ASYNC_RECV, COMM_ASYNC_SEND, COMM_IPROBE, COMM_TEST, COMM_WAIT,
      /* alphabetical, but the MUTEX_LOCK_NOMC that is not used in MC mode and thus only used for debug msg */
      MUTEX_ASYNC_LOCK, MUTEX_TEST, MUTEX_TRYLOCK, MUTEX_UNLOCK, MUTEX_WAIT, MUTEX_LOCK_NOMC,
      /* alphabetical ordering of SEM transitions */
      SEM_ASYNC_LOCK, SEM_UNLOCK, SEM_WAIT, SEM_LOCK_NOMC,
      /* alphabetical ordering of condition variable transitions */
      CONDVAR_ASYNC_LOCK, CONDVAR_BROADCAST, CONDVAR_SIGNAL, CONDVAR_WAIT, CONDVAR_NOMC,
      /* UNKNOWN must be last */
      UNKNOWN);
  Type type_ = Type::UNKNOWN;

  aid_t aid_ = 0;

  /** The user function call that caused this transition to exist. Format: >>filename:line:function()<< */
  std::string call_location_ = "";

  /* Which transition was executed for this simcall
   *
   * Some simcalls can lead to different transitions:
   *
   * * waitany/testany can trigger on different messages;
   *
   * * random can produce different values.
   */
  int times_considered_ = 0;

  Transition() = default;
  Transition(Type type, aid_t issuer, int times_considered)
      : type_(type), aid_(issuer), times_considered_(times_considered)
  {
  }
  virtual ~Transition();

  /** Returns a textual representation of the transition. Pointer adresses are omitted if verbose=false */
  virtual std::string to_string(bool verbose = false) const;
  /** Returns something like >>label = "desc", color = c<< to describe the transition in dot format */
  virtual std::string dot_string() const;

  std::string const& get_call_location() const { return call_location_; }

  /* Moves the application toward a path that was already explored, but don't change the current transition */
  void replay(RemoteApp& app) const;

  virtual bool depends(const Transition* other) const { return true; }

  /* Transitions can be co-enabled if there can exist a state in which one actor wants to do one of them
     and an other actor wants to do the other one */
  virtual bool can_be_co_enabled(const Transition* other) const
  {
    if (other->type_ < type_)
      return other->can_be_co_enabled(this);
    else
      return other->aid_ != aid_;
  }

  /**
   The reversible race detector should only be used if we already have the assumption
   this <* other (see Source set: a foundation for ODPOR). In particular this means that :
   - this -->_E other
   - proc(this) != proc(other)
   - there is no event e s.t. this --> e --> other

    @note: It is safe to assume that there is indeed a race between the two events in the execution;
     indeed, the question the method answers is only sensible in the context of a race

     @note: Be carefull when implementing new transition, this method is NOT symmetrical (meaning that
     this.reversible_race(other) may be different from other.reversible_race(this)). Be sure to implement
     both way or the reduction might fail.
  */
  virtual bool reversible_race(const Transition* other) const
  {
    xbt_die("%s unimplemented for %s", __func__, to_c_str(type_));
  }

  /* Returns the total amount of transitions executed so far (for statistics) */
  static unsigned long get_executed_transitions() { return executed_transitions_; }
  /* Returns the total amount of transitions replayed so far while backtracing (for statistics) */
  static unsigned long get_replayed_transitions() { return replayed_transitions_; }
};

/** Make a new transition from serialized description */
Transition* deserialize_transition(aid_t issuer, int times_considered, std::stringstream& stream);

} // namespace simgrid::mc

#endif
