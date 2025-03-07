/* Copyright (c) 2013-2025. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "cpu_ti.hpp"
#include "simgrid/kernel/routing/NetZoneImpl.hpp"
#include "simgrid/s4u/Engine.hpp"
#include "src/kernel/EngineImpl.hpp"
#include "src/kernel/resource/profile/Event.hpp"
#include "src/kernel/resource/profile/Profile.hpp"
#include "src/simgrid/math_utils.h"
#include "xbt/asserts.h"

#include <algorithm>
#include <memory>

constexpr double EPSILON = 0.000000001;

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(cpu_ti, res_cpu, "CPU resource, Trace Integration model");

namespace simgrid::kernel::resource {

/*********
 * Trace *
 *********/

CpuTiProfile::CpuTiProfile(const profile::Profile* profile)
{
  double integral                                = 0;
  double time                                    = 0;
  double prev_value                              = 1;
  const std::vector<profile::DatedValue>& events = profile->get_event_list();
  xbt_assert(not events.empty());
  unsigned long nb_points = events.size() + 1;
  time_points_.reserve(nb_points);
  integral_.reserve(nb_points);
  for (auto const& val : events) {
    time += val.date_;
    integral += val.date_ * prev_value;
    time_points_.push_back(time);
    integral_.push_back(integral);
    prev_value = val.value_;
  }

  double delay = profile->get_repeat_delay() + events.at(0).date_;

  xbt_assert(events.back().value_ == prev_value, "Profiles need to end as they start");
  time += delay;
  integral += delay * prev_value;

  time_points_.push_back(time);
  integral_.push_back(integral);
}

/**
 * @brief Integrate trace
 *
 * Wrapper around profile_->integrate_simple() to get
 * the cyclic effect.
 *
 * @param a      Begin of interval
 * @param b      End of interval
 * @return the integrate value. -1 if an error occurs.
 */
double CpuTiTmgr::integrate(double a, double b) const
{
  xbt_assert(a >= 0.0 && a <= b,
             "Error, invalid integration interval [%.2f,%.2f]. You probably have a task executing with negative "
             "computation amount. Check your code.",
             a, b);
  if (fabs(a - b) < EPSILON)
    return 0.0;

  if (type_ == Type::FIXED) {
    return (b - a) * value_;
  }

  double a_index;
  if (fabs(ceil(a / last_time_) - a / last_time_) < EPSILON)
    a_index = 1 + ceil(a / last_time_);
  else
    a_index = ceil(a / last_time_);
  double b_index = floor(b / last_time_);

  if (a_index > b_index) { /* Same chunk */
    return profile_->integrate_simple(a - (a_index - 1) * last_time_, b - b_index * last_time_);
  }

  double first_chunk  = profile_->integrate_simple(a - (a_index - 1) * last_time_, last_time_);
  double middle_chunk = (b_index - a_index) * total_;
  double last_chunk   = profile_->integrate_simple(0.0, b - b_index * last_time_);

  XBT_DEBUG("first_chunk=%.2f  middle_chunk=%.2f  last_chunk=%.2f\n", first_chunk, middle_chunk, last_chunk);

  return (first_chunk + middle_chunk + last_chunk);
}

/**
 * @brief Auxiliary function to compute the integral between a and b.
 *     It simply computes the integrals at point a and b and returns the difference between them.
 * @param a  Initial point
 * @param b  Final point
 */
double CpuTiProfile::integrate_simple(double a, double b) const
{
  return integrate_simple_point(b) - integrate_simple_point(a);
}

/**
 * @brief Auxiliary function to compute the integral at point a.
 * @param a        point
 */
double CpuTiProfile::integrate_simple_point(double a) const
{
  double integral = 0;
  double a_aux    = a;
  long ind        = binary_search(time_points_, a);
  integral += integral_[ind];

  XBT_DEBUG("a %f ind %ld integral %f ind + 1 %f ind %f time +1 %f time %f", a, ind, integral, integral_[ind + 1],
            integral_[ind], time_points_[ind + 1], time_points_[ind]);
  double_update(&a_aux, time_points_[ind], sg_precision_workamount * sg_precision_timing);
  if (a_aux > 0)
    integral +=
        ((integral_[ind + 1] - integral_[ind]) / (time_points_[ind + 1] - time_points_[ind])) * (a - time_points_[ind]);
  XBT_DEBUG("Integral a %f = %f", a, integral);

  return integral;
}

/**
 * @brief Computes the time needed to execute "amount" on cpu.
 *
 * Here, amount can span multiple trace periods
 *
 * @param a        Initial time
 * @param amount  Amount to be executed
 * @return  End time
 */
double CpuTiTmgr::solve(double a, double amount) const
{
  /* Fix very small negative numbers */
  if ((a < 0.0) && (a > -EPSILON)) {
    a = 0.0;
  }
  if ((amount < 0.0) && (amount > -EPSILON)) {
    amount = 0.0;
  }

  /* Sanity checks */
  xbt_assert(a >= 0.0 && amount >= 0.0,
             "Error, invalid parameters [a = %.2f, amount = %.2f]. "
             "You probably have a task executing with negative computation amount. Check your code.",
             a, amount);

  /* At this point, a and amount are positive */
  if (amount < EPSILON)
    return a;

  /* Is the trace fixed ? */
  if (type_ == Type::FIXED) {
    return (a + (amount / value_));
  }

  XBT_DEBUG("amount %f total %f", amount, total_);
  /* Reduce the problem to one where amount <= trace_total */
  double quotient       = floor(amount / total_);
  double reduced_amount = total_ * ((amount / total_) - floor(amount / total_));
  double reduced_a      = a - last_time_ * static_cast<int>(floor(a / last_time_));

  XBT_DEBUG("Quotient: %g reduced_amount: %f reduced_a: %f", quotient, reduced_amount, reduced_a);

  /* Now solve for new_amount which is <= trace_total */
  XBT_DEBUG("Solve integral: [%.2f, amount=%.2f]", reduced_a, reduced_amount);

  double amount_till_end = integrate(reduced_a, last_time_);
  double reduced_b       = amount_till_end > reduced_amount
                               ? profile_->solve_simple(reduced_a, reduced_amount)
                               : last_time_ + profile_->solve_simple(0.0, reduced_amount - amount_till_end);

  /* Re-map to the original b and amount */
  return last_time_ * floor(a / last_time_) + (quotient * last_time_) + reduced_b;
}

/**
 * @brief Auxiliary function to solve integral.
 *  It returns the date when the requested amount of flops is available
 * @param a        Initial point
 * @param amount  Amount of flops
 * @return The date when amount is available.
 */
double CpuTiProfile::solve_simple(double a, double amount) const
{
  double integral_a = integrate_simple_point(a);
  long ind          = binary_search(integral_, integral_a + amount);
  double time       = time_points_[ind];
  time += (integral_a + amount - integral_[ind]) /
          ((integral_[ind + 1] - integral_[ind]) / (time_points_[ind + 1] - time_points_[ind]));

  return time;
}

/**
 * @brief Auxiliary function to update the CPU speed scale.
 *
 *  This function uses the trace structure to return the speed scale at the determined time a.
 * @param a        Time
 * @return CPU speed scale
 */
double CpuTiTmgr::get_power_scale(double a) const
{
  double reduced_a        = a - floor(a / last_time_) * last_time_;
  long point              = CpuTiProfile::binary_search(profile_->get_time_points(), reduced_a);
  profile::DatedValue val = speed_profile_->get_event_list().at(point);
  return val.value_;
}

/**
 * @brief Creates a new integration trace from a tmgr_trace_t
 *
 * @param  speed_trace    CPU availability trace
 * @param  value          Percentage of CPU speed available (useful to fixed tracing)
 * @return  Integration trace structure
 */
CpuTiTmgr::CpuTiTmgr(kernel::profile::Profile* speed_profile, double value) : speed_profile_(speed_profile)
{
  double total_time = 0.0;
  profile_.reset(nullptr);

  /* no availability file, fixed trace */
  if (not speed_profile) {
    value_ = value;
    XBT_DEBUG("No availability trace. Constant value = %f", value);
    return;
  }

  xbt_assert(speed_profile->is_repeating());

  /* only one point available, fixed trace */
  if (speed_profile->get_event_list().size() == 1) {
    value_ = speed_profile->get_event_list().front().value_;
    return;
  }

  type_ = Type::DYNAMIC;

  /* count the total time of trace file */
  for (auto const& val : speed_profile->get_event_list())
    total_time += val.date_;
  total_time += speed_profile->get_repeat_delay();

  profile_   = std::make_unique<CpuTiProfile>(speed_profile);
  last_time_ = total_time;
  total_     = profile_->integrate_simple(0, total_time);

  XBT_DEBUG("Total integral %f, last_time %f ", total_, last_time_);
}

/**
 * @brief Binary search in array.
 *  It returns the last point of the interval in which "a" is.
 * @param array    Array
 * @param a        Value to search
 * @return Index of point
 */
long CpuTiProfile::binary_search(const std::vector<double>& array, double a)
{
  if (array[0] > a)
    return 0;
  auto pos = std::upper_bound(begin(array), end(array), a);
  return std::distance(begin(array), pos) - 1;
}

/*********
 * Model *
 *********/

void CpuTiModel::create_pm_models()
{
  auto cpu_model_pm = std::make_shared<CpuTiModel>("Cpu_TI");
  auto* engine      = EngineImpl::get_instance();
  engine->add_model(cpu_model_pm);
  engine->get_netzone_root()->set_cpu_pm_model(cpu_model_pm);
}

CpuImpl* CpuTiModel::create_cpu(s4u::Host* host, const std::vector<double>& speed_per_pstate)
{
  return (new CpuTi(host, speed_per_pstate))->set_model(this);
}

double CpuTiModel::next_occurring_event(double now)
{
  double min_action_duration = -1;

  /* iterates over modified cpus to update share resources */
  for (auto it = std::begin(modified_cpus_); it != std::end(modified_cpus_);) {
    CpuTi& cpu = *it;
    ++it; // increment iterator here since the following call to ti.update_actions_finish_time() may invalidate it
    cpu.update_actions_finish_time(now);
  }

  /* get the min next event if heap not empty */
  if (not get_action_heap().empty())
    min_action_duration = get_action_heap().top_date() - now;

  XBT_DEBUG("Share resources, min next event date: %f", min_action_duration);

  return min_action_duration;
}

void CpuTiModel::update_actions_state(double now, double /*delta*/)
{
  while (not get_action_heap().empty() && double_equals(get_action_heap().top_date(), now, sg_precision_timing)) {
    auto* action = static_cast<CpuTiAction*>(get_action_heap().pop());
    XBT_DEBUG("Action %p: finish", action);
    action->finish(Action::State::FINISHED);
    /* update remaining amount of all actions */
    action->cpu_->update_remaining_amount(EngineImpl::get_clock());
  }
}

/************
 * Resource *
 ************/
CpuTi::CpuTi(s4u::Host* host, const std::vector<double>& speed_per_pstate) : CpuImpl(host, speed_per_pstate)
{
  speed_.peak = speed_per_pstate.front();
  XBT_DEBUG("CPU create: peak=%f", speed_.peak);

  speed_integrated_trace_ = new CpuTiTmgr(nullptr, 1 /*scale*/);
}

CpuTi::~CpuTi()
{
  set_modified(false);
  delete speed_integrated_trace_;
}

void CpuTi::turn_off()
{
  /* Skip CpuImpl::turn_off() that marks the actions as failing, as it seems to be done otherwise in CPU TI.
   * So, just avoid the segfault for now.
   *
   * TODO: a proper solution would be to understand and adapt the way actions are marked FAILED in here,
   * and adapt it to align with the other resources. */
  Resource::turn_off();
}

CpuImpl* CpuTi::set_speed_profile(kernel::profile::Profile* profile)
{
  delete speed_integrated_trace_;
  speed_integrated_trace_ = new CpuTiTmgr(profile, speed_.scale);

  /* add a fake trace event if periodicity == 0 */
  if (profile && profile->get_event_list().size() > 1) {
    kernel::profile::DatedValue val = profile->get_event_list().back();
    if (val.date_ < 1e-12) {
      auto* prof   = profile::ProfileBuilder::from_void();
      speed_.event = prof->schedule(&profile::future_evt_set, this);
    }
  }
  return this;
}

void CpuTi::apply_event(kernel::profile::Event* event, double value)
{
  if (event == speed_.event) {
    XBT_DEBUG("Speed changed in trace! New fixed value: %f", value);

    /* update remaining of actions and put in modified cpu list */
    update_remaining_amount(EngineImpl::get_clock());

    set_modified(true);

    delete speed_integrated_trace_;
    speed_integrated_trace_ = new CpuTiTmgr(value);

    speed_.scale = value;
    tmgr_trace_event_unref(&speed_.event);

  } else if (event == get_state_event()) {
    if (value > 0) {
      if (not is_on()) {
        XBT_VERB("Restart actors on host %s", get_iface()->get_cname());
        get_iface()->turn_on();
      }
    } else {
      get_iface()->turn_off();

      /* put all action running on cpu to failed */
      double now = EngineImpl::get_clock();
      for (CpuTiAction& action : action_set_) {
        if (action.get_state() == Action::State::INITED || action.get_state() == Action::State::STARTED ||
            action.get_state() == Action::State::IGNORED) {
          action.set_finish_time(now);
          action.set_state(Action::State::FAILED);
          get_model()->get_action_heap().remove(&action);
        }
      }
    }
    unref_state_event();

  } else {
    xbt_die("Unknown event!\n");
  }
}

/** Update the actions that are running on this CPU (which was modified recently) */
void CpuTi::update_actions_finish_time(double now)
{
  /* update remaining amount of actions */
  update_remaining_amount(now);

  /* Compute the sum of priorities for the actions running on that CPU */
  sum_priority_ = 0.0;
  for (CpuTiAction const& action : action_set_) {
    /* action not running, skip it */
    if (action.get_state_set() != get_model()->get_started_action_set())
      continue;

    /* bogus priority, skip it */
    if (action.get_sharing_penalty() <= 0)
      continue;

    /* action suspended, skip it */
    if (not action.is_running())
      continue;

    sum_priority_ += 1.0 / action.get_sharing_penalty();
  }

  for (CpuTiAction& action : action_set_) {
    double min_finish = NO_MAX_DURATION;
    /* action not running, skip it */
    if (action.get_state_set() != get_model()->get_started_action_set())
      continue;

    /* verify if the action is really running on cpu */
    if (action.is_running() && action.get_sharing_penalty() > 0) {
      /* total area needed to finish the action. Used in trace integration */
      double total_area = (action.get_remains() * sum_priority_ * action.get_sharing_penalty()) / speed_.peak;

      action.set_finish_time(speed_integrated_trace_->solve(now, total_area));
      /* verify which event will happen before (max_duration or finish time) */
      if (action.get_max_duration() != NO_MAX_DURATION &&
          action.get_start_time() + action.get_max_duration() < action.get_finish_time())
        min_finish = action.get_start_time() + action.get_max_duration();
      else
        min_finish = action.get_finish_time();
    } else {
      /* put the max duration time on heap */
      if (action.get_max_duration() != NO_MAX_DURATION)
        min_finish = action.get_start_time() + action.get_max_duration();
    }
    /* add in action heap */
    if (min_finish != NO_MAX_DURATION)
      get_model()->get_action_heap().update(&action, min_finish, ActionHeap::Type::unset);
    else
      get_model()->get_action_heap().remove(&action);

    XBT_DEBUG("Update finish time: Cpu(%s) Action: %p, Start Time: %f Finish Time: %f Max duration %f", get_cname(),
              &action, action.get_start_time(), action.get_finish_time(), action.get_max_duration());
  }
  /* remove from modified cpu */
  set_modified(false);
}

bool CpuTi::is_used() const
{
  return not action_set_.empty();
}

double CpuTi::get_speed_ratio()
{
  speed_.scale = speed_integrated_trace_->get_power_scale(EngineImpl::get_clock());
  return CpuImpl::get_speed_ratio();
}

/** @brief Update the remaining amount of actions */
void CpuTi::update_remaining_amount(double now)
{
  /* already up to date */
  if (last_update_ >= now)
    return;

  /* compute the integration area */
  double area_total = speed_integrated_trace_->integrate(last_update_, now) * speed_.peak;
  XBT_DEBUG("Flops total: %f, Last update %f", area_total, last_update_);
  for (CpuTiAction& action : action_set_) {
    /* action not running, skip it */
    if (action.get_state_set() != get_model()->get_started_action_set())
      continue;

    /* bogus priority, skip it */
    if (action.get_sharing_penalty() <= 0)
      continue;

    /* action suspended, skip it */
    if (not action.is_running())
      continue;

    /* action don't need update */
    if (action.get_start_time() >= now)
      continue;

    /* skip action that are finishing now */
    if (action.get_finish_time() >= 0 && action.get_finish_time() <= now)
      continue;

    /* update remaining */
    action.update_remains(area_total / (sum_priority_ * action.get_sharing_penalty()));
    XBT_DEBUG("Update remaining action(%p) remaining %f", &action, action.get_remains_no_update());
  }
  last_update_ = now;
}

CpuAction* CpuTi::execution_start(double size, double user_bound)
{
  XBT_IN("(%s,%g)", get_cname(), size);
  xbt_assert(user_bound <= 0, "Invalid user bound (%lf) in CPU TI model", user_bound);
  auto* action = new CpuTiAction(this, size);

  action_set_.push_back(*action); // Actually start the action

  XBT_OUT();
  return action;
}

CpuAction* CpuTi::sleep(double duration)
{
  if (duration > 0)
    duration = std::max(duration, sg_precision_timing);

  XBT_IN("(%s,%g)", get_cname(), duration);
  auto* action = new CpuTiAction(this, 1.0);

  action->set_max_duration(duration);
  action->set_suspend_state(Action::SuspendStates::SLEEPING);
  if (duration == NO_MAX_DURATION)
    action->set_state(Action::State::IGNORED);

  action_set_.push_back(*action);

  XBT_OUT();
  return action;
}

void CpuTi::set_modified(bool modified)
{
  CpuTiList& modified_cpus = static_cast<CpuTiModel*>(get_model())->modified_cpus_;
  if (modified) {
    if (not cpu_ti_hook.is_linked()) {
      modified_cpus.push_back(*this);
    }
  } else {
    if (cpu_ti_hook.is_linked())
      xbt::intrusive_erase(modified_cpus, *this);
  }
}

/**********
 * Action *
 **********/

CpuTiAction::CpuTiAction(CpuTi* cpu, double cost) : CpuAction(cpu->get_model(), cost, not cpu->is_on()), cpu_(cpu)
{
  cpu_->set_modified(true);
}
CpuTiAction::~CpuTiAction()
{
  /* remove from action_set */
  if (action_ti_hook.is_linked())
    xbt::intrusive_erase(cpu_->action_set_, *this);
  /* remove from heap */
  get_model()->get_action_heap().remove(this);
  cpu_->set_modified(true);
}

void CpuTiAction::set_state(Action::State state)
{
  CpuAction::set_state(state);
  cpu_->set_modified(true);
}

void CpuTiAction::cancel()
{
  this->set_state(Action::State::FAILED);
  get_model()->get_action_heap().remove(this);
  cpu_->set_modified(true);
}

void CpuTiAction::suspend()
{
  XBT_IN("(%p)", this);
  if (is_running()) {
    set_suspend_state(Action::SuspendStates::SUSPENDED);
    get_model()->get_action_heap().remove(this);
    cpu_->set_modified(true);
  }
  XBT_OUT();
}

void CpuTiAction::resume()
{
  XBT_IN("(%p)", this);
  if (is_suspended()) {
    set_suspend_state(Action::SuspendStates::RUNNING);
    cpu_->set_modified(true);
  }
  XBT_OUT();
}

void CpuTiAction::set_sharing_penalty(double sharing_penalty)
{
  XBT_IN("(%p,%g)", this, sharing_penalty);
  set_sharing_penalty_no_update(sharing_penalty);
  cpu_->set_modified(true);
  XBT_OUT();
}

double CpuTiAction::get_remains()
{
  XBT_IN("(%p)", this);
  cpu_->update_remaining_amount(EngineImpl::get_clock());
  XBT_OUT();
  return get_remains_no_update();
}

} // namespace simgrid::kernel::resource
