/* Copyright (c) 2007-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_ODPOR_EXECUTION_HPP
#define SIMGRID_MC_ODPOR_EXECUTION_HPP

#include "src/mc/api/ClockVector.hpp"
#include "src/mc/explo/odpor/odpor_forward.hpp"
#include "src/mc/mc_forward.hpp"
#include "src/mc/mc_record.hpp"
#include "src/mc/transition/Transition.hpp"

#include <list>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

namespace simgrid::mc::odpor {

std::vector<std::string> get_textual_trace(const PartialExecution& w);
std::string one_string_textual_trace(const PartialExecution& w);

/**
 * @brief The occurrence of a transition in an execution
 *
 * An execution is set of *events*, where each element represents
 * the occurrence or execution of the `i`th step of a particular
 * actor `j`
 */
class Event {
  std::pair<std::shared_ptr<Transition>, ClockVector> contents_;

  // Have reversible races between this event and its prefix already been computed ?
  mutable bool race_considered_ = false;

public:
  Event()                        = default;
  Event(Event&&)                 = default;
  Event(const Event&)            = default;
  Event& operator=(const Event&) = default;
  explicit Event(std::pair<std::shared_ptr<Transition>, ClockVector> pair) : contents_(std::move(pair)) {}

  std::shared_ptr<Transition> get_transition() const { return std::get<0>(contents_); }
  const ClockVector& get_clock_vector() const { return std::get<1>(contents_); }

  bool has_race_been_computed() const { return race_considered_; }
  void consider_races() const { race_considered_ = true; }
};

/**
 * @brief An ordered sequence of transitions which describe
 * the evolution of a process undergoing model checking
 *
 * An execution conceptually is just a string of actors
 * ids (e.g. "1.2.3.1.2.2.1.1"), where the `i`th occurrence
 * of actor id `j` corresponds to the `i`th action executed
 * by the actor with id `j` (viz. the `i`th step of actor `j`).
 * Executions can stand alone on their own or can extend
 * the execution of other sequences
 *
 * Executions are conceived based on the following papers:
 * 1. "Source Sets: A Foundation for Optimal Dynamic Partial Order Reduction"
 * by Abdulla et al.
 *
 * In addition to representing an actual steps taken,
 * an execution keeps track of the "happens-before"
 * relation among the transitions in the execution
 * by following the procedure outlined in section 4 of the
 * original DPOR paper with clock vectors.
 * As new transitions are added to the execution, clock vectors are
 * computed as appropriate and associated with the corresponding position
 * in the execution. This allows us to determine “happens-before” in
 * constant-time between points in the execution (called events
 * [which is unfortunately the same name used in UDPOR for a slightly
 * different concept]), albeit for an up-front cost of traversing the
 * execution stack. The happens-before relation is important in many
 * places in SDPOR and ODPOR.
 *
 * @note: For more nuanced happens-before relations, clock
 * vectors may not always suffice. Clock vectors work
 * well with transition-based dependencies like that used in
 * SimGrid; but to have a more refined independence relation,
 * an event-based dependency approach is needed. See the section 2
 * in the ODPOR paper [1] concerning event-based dependencies and
 * how the happens-before relation can be refined in a
 * computation model much like that of SimGrid. In fact, the same issue
 * arrises with UDPOR with context-sensitive dependencies:
 * the two concepts are analogous if not identical
 */
class Execution {
public:
  using EventHandle = uint32_t;

private:
  std::vector<Event> contents_;
  std::vector<std::vector<EventHandle>> skip_list_ = {{}};
  Execution(std::vector<Event>&& contents) : contents_(std::move(contents)) {}

  static PartialExecution preallocated_partial_execution_;

public:
  Execution()                            = default;
  Execution(const Execution&)            = default;
  Execution& operator=(Execution const&) = default;
  Execution(Execution&&)                 = default;
  Execution(const PartialExecution&);

  std::vector<std::string> get_textual_trace() const;
  std::string get_one_string_textual_trace() const;

  size_t size() const { return this->contents_.size(); }
  bool empty() const { return this->contents_.empty(); }
  auto begin() const { return this->contents_.begin(); }
  auto end() const { return this->contents_.end(); }

  /**
   * @brief Computes the "core" portion the SDPOR algorithm,
   * viz. the intersection of the backtracking set and the
   * set of initials with respect to the *last* event added
   * to the execution
   *
   * The "core" portion of the SDPOR algorithm is found on
   * lines 6-9 of the pseudocode:
   *
   * 6 | let E' := pre(E, e)
   * 7 | let v :=  notdep(e, E).p
   * 8 | if I_[E'](v) ∩ backtrack(E') = empty then
   * 9 |    --> add some q in I_[E'](v) to backtrack(E')
   *
   * This method computes all of the lines simultaneously,
   * returning the set `I_[E'](v)` if condition on line 8 holds.
   * The event `e` and the set `backtrack(E')` are the provided
   * arguments to the method.
   *
   * @param e the event with respect to which to determine
   * whether a backtrack point needs to be added for the
   * prefix corresponding to the execution prior to `e`
   *
   * @param backtrack_set The set of actors which should
   * not be considered for selection as an SDPOR initial.
   * While this set need not necessarily correspond to the
   * backtrack set `backtrack(E')`, doing so provides what
   * is expected for SDPOR
   *
   * See the SDPOR algorithm pseudocode in [1] for more
   * details for the context of the function.
   *
   * @precondition: This method assumes that events `e` and
   * `e' := get_latest_event_handle()` are in a *reversible* race,
   * as is explicitly the case in SDPOR
   *
   * @returns a set of actors not already contained in `backtrack_set`
   * which serve as an initials to reverse the race between `e`
   * and `e' := get_latest_event_handle()`; that is, an initial that is
   * not already contained in the set `backtrack_set`.
   */
  std::unordered_set<aid_t> get_missing_source_set_actors_from(EventHandle e,
                                                               const std::unordered_set<aid_t>& backtrack_set) const;

  /**
   * @brief Computes the analogous lines from the SDPOR algorithm
   * in the ODPOR algorithm, viz. the intersection of the sleep set
   * and the set of weak initials with respect to the given pair
   * of racing events
   *
   * This method computes lines 4-6 of the ODPOR pseudocode, viz.:
   *
   * 4 | let E' := pre(E, e)
   * 5 | let v := notdep(e, E).e'^
   * 6 | if sleep(E') ∩ WI_[E'](v) = empty then
   * 7 |   --> wut(E') := insert_[E'](v, wut(E'))
   *
   * The sequence `v` is computed and returned as needed, based on whether
   * the check on line 6 passes.
   *
   * @precondition: This method assumes that events `e` and
   * `e_prime` are in a *reversible* race, as is the case
   * in ODPOR.
   *
   * @returns a partial execution `v := notdep(e, E)` (where `E` refers
   * to this execution) that should be inserted into a wakeup tree with
   * respect to this execution if `sleep(E') ∩ WI_[E'](v) = empty`, and
   * `std::nullopt` otherwise
   */
  std::optional<PartialExecution> get_odpor_extension_from(EventHandle e, EventHandle e_prime,
                                                           const SleepSetState& state_at_e,
                                                           aid_t actor_after_e = -1) const;

  /**
   * @brief For a given sequence of actors `v` and a sequence of transitions `w`,
   * computes the sequence, if any, that should be inserted as a child in wakeup tree for
   * this execution
   *
   * Recall that the procedure for implementing the insertion
   * is outlined in section 6.2 of Abdulla et al. 2017 as follows:
   *
   * | Let `v` be the smallest (w.r.t to "<") sequence in [the tree] B
   * | such that `v ~_[E] w`. If `v` is a leaf node, the tree can be left
   * | unmodified.
   * |
   * | Otherwise let `w'` be the shortest sequence such that `w [=_[E] v.w'`
   * | and add `v.w'` as a new leaf, ordered after all already existing nodes
   * | of the form `v.w''`
   *
   * The procedure for determining whether `v ~_[E] w` is given as Lemma 4.6 of
   * Abdulla et al. 2017:
   *
   * | The relation `v ~_[E] w` holds if either
   * | (1) v = <>, or
   * | (2) v := p.v' and either
   * |     (a) p in I_[E](w) and `v' ~_[E.p] (w \ p)`
   * |     (b) E ⊢ p ◊ w and `v' ~_[E.p] w`
   *
   * This method computes the result `v.w'` as needed (viz. only if `v ~_[E] w`
   * with respect to this execution `E`). The implementation takes advantage
   * of the fact that determining whether `v ~_[E] w` yields "for free" the
   * the shortest such `w'` we are looking for; if we ultimately determine
   * that `v ~_[E] w`, the work we did to do so leaves us precisely with `w'`,
   * so we can simply prepend `v` to it and call it a day
   *
   * @precondition: This method assumes that `E.v` is a valid execution, viz.
   * that the events of `E` are sufficient to enabled `v_0` and that
   * `v_0, ..., v_{i - 1}` are sufficient to enable `v_i`. This is the
   * case when e.g. `v := notdep(e, E).p` for example in ODPOR
   *
   * @returns a partial execution `v.w'` that should be inserted
   * as a child of a wakeup tree node representing the sequence `v`
   * if `v ~_[E] w`, or `std::nullopt` if that relation does not hold
   * between the two sequences `v` and `w`
   */
  static std::optional<PartialExecution> get_shortest_odpor_sq_subset_insertion(const PartialExecution& v,
                                                                                const PartialExecution& w);

  /**
   * @brief For a given sequence `w`, determines whether p in I_[E](w)
   *
   * @note: You may notice that some of the other methods compute this
   * value as well. What we notice, though, in those cases is that
   * we are repeatedly asking about initials with respect to an execution.
   * It is better, then, to bunch the work together in those cases to
   * get asymptotically better results (e.g. instead of calling with all
   * `N` actors, we can process them "in-parallel" as is done with the
   * computation of SDPOR initials)
   */
  static bool is_initial_after_execution_of(const PartialExecution& w, aid_t p);

  /**
   * @brief Determines whether `E ⊢ p ◊ w` given the next action taken by `p`
   */
  static bool is_independent_with_execution_of(const PartialExecution& w, std::shared_ptr<Transition> next_E_p);

  /**
   * @brief Determines the event associated with the given handle `handle`
   */
  const Event& get_event_with_handle(EventHandle handle) const { return contents_[handle]; }

  /**
   * @brief Determines the actor associated with the given event handle `handle`
   */
  aid_t get_actor_with_handle(EventHandle handle) const { return get_event_with_handle(handle).get_transition()->aid_; }

  /**
   * @brief Determines the transition associated with the given handle `handle`
   */
  const Transition* get_transition_for_handle(EventHandle handle) const
  {
    return get_event_with_handle(handle).get_transition().get();
  }

  /**
   * @brief Returns a handle to the newest event of the execution, if such an event exists
   *
   * @returns the handle to the last event of the execution.
   * If the sequence is empty, no such handle exists and the
   * method returns `std::nullopt`
   */
  std::optional<EventHandle> get_latest_event_handle() const
  {
    return contents_.empty() ? std::nullopt : std::optional<EventHandle>{static_cast<EventHandle>(size() - 1)};
  }

  /**
   * @brief Returns a set of events which are in
   * "immediate conflict" (according to the definition given
   * in the ODPOR paper) with the given event
   *
   * Two events `e` and `e'` in an execution `E` are said to
   * *race* iff
   *
   * 1. `proc(e) != proc(e')`; that is, the events correspond to
   * the execution of different actors
   * 2. `e -->_E e'` and there is no `e''` in `E` such that
   *  `e -->_E e''` and `e'' -->_E e'`; that is, the two events
   * "happen-before" one another in `E` and no other event in
   * `E` "happens-between" `e` and `e'`
   *
   * @param handle the event with respect to which races are
   * computed
   * @returns a set of event handles, each element of which is an
   * event in this execution which is in a *race* with event `handle`
   */
  std::list<EventHandle> get_racing_events_of(EventHandle handle) const;

  /**
   * @brief Returns a set of events which are in a reversible
   * race with the given event handle `handle`
   *
   * Two events `e` and `e'` in an execution `E` are said to
   * be in a *reversible race* iff
   *
   * 1. `e` and `e'` race
   * 2. In any equivalent execution sequence `E'` to `E`
   * where `e` occurs immediately before `e'`, the actor
   * running `e'` was enabled in the state prior to `e`
   *
   * @param handle the event with respect to which
   * reversible races are computed
   * @returns a set of event handles, each element of which is an event
   * in this execution which is in a *reversible race* with event `handle`
   */
  std::list<EventHandle> get_reversible_races_of(EventHandle handle) const;

  /**
   * @brief Computes `pre(e, E)` as described in ODPOR [1]
   *
   * The execution `pre(e, E)` for an event `e` in an
   * execution `E` is the contiguous prefix of events
   * `E' <= E` up to but excluding the event `e` itself.
   * Roughly speaking, the prefix intuitively represents
   * the "history" of causes which permitted event `e`
   * to exist
   */
  Execution get_prefix_before(EventHandle) const;

  /**
   * @brief Whether the event represented by `e1`
   * "happens-before" the event represented by
   * `e2` in the context of this execution
   *
   * In the terminology of the ODPOR paper,
   * this function computes
   *
   * `e1 --->_E e2`
   *
   * where `E` is this execution
   *
   * @note: The happens-before relation computed by this
   * execution is "coarse" in the sense that context-sensitive
   * independence is not exploited. To include such context-sensitive
   * dependencies requires a new method of keeping track of
   * the happens-before procedure, which is nontrivial...
   */
  bool happens_before(EventHandle e1, EventHandle e2) const;

  /**
   * @brief Whether the event represented by `e`
   * "happens-before" the process p
   * in the context of this execution
   *
   * In the terminology of the DPOR paper,
   * this function computes
   *
   * `e --->_E p`
   *
   * where `E` is this execution
   */
  bool happens_before_process(EventHandle e, aid_t p) const;

  /**
   * @brief Extends the execution by one more step
   *
   * Intutively, pushing a transition `t` onto execution `E`
   * is equivalent to making the execution become (using the
   * notation of [1]) `E.proc(t)` where `proc(t)` is the
   * actor which executed transition `t`.
   */
  void push_transition(std::shared_ptr<Transition>, bool are_we_restoring_execution = false);

  /**
   * @brief Shorten the execution by one step
   *
   */
  void remove_last_event();

  /**
   * @brief Extends the execution by a sequence of steps
   *
   * This method has the equivalent effect of pushing the
   * transitions of the partial execution one-by-one onto
   * the execution
   */
  void push_partial_execution(const PartialExecution&);

  /**
   * @brief Returns wether a given transition is in WI(v)
   *
   */
  static bool is_in_weak_initial_of(Transition*, const PartialExecution&);
};

class MazurkiewiczTraces {

  static std::set<PartialExecution> classes_;

  static bool are_equivalent(const PartialExecution&, const PartialExecution&);

public:
  MazurkiewiczTraces() = delete;

  static void record_new_execution(const Execution&);
};

} // namespace simgrid::mc::odpor
#endif
