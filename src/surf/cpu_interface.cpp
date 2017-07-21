/* Copyright (c) 2013-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/dynar.h>
#include "cpu_interface.hpp"
#include "src/instr/instr_private.h" // TRACE_is_enabled(). FIXME: remove by subscribing tracing to the surf signals

XBT_LOG_EXTERNAL_CATEGORY(surf_kernel);
XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_cpu, surf, "Logging specific to the SURF cpu module");

simgrid::surf::CpuModel *surf_cpu_model_pm;
simgrid::surf::CpuModel *surf_cpu_model_vm;

namespace simgrid {
namespace surf {

/*********
 * Model *
 *********/

void CpuModel::updateActionsStateLazy(double now, double /*delta*/)
{
  while ((xbt_heap_size(getActionHeap()) > 0)
         && (double_equals(xbt_heap_maxkey(getActionHeap()), now, sg_surf_precision))) {

    CpuAction *action = static_cast<CpuAction*>(xbt_heap_pop(getActionHeap()));
    XBT_CDEBUG(surf_kernel, "Something happened to action %p", action);
    if (TRACE_is_enabled()) {
      Cpu *cpu = static_cast<Cpu*>(lmm_constraint_id(lmm_get_cnst_from_var(getMaxminSystem(), action->getVariable(), 0)));
      TRACE_surf_host_set_utilization(cpu->cname(), action->getCategory(), lmm_variable_getvalue(action->getVariable()),
                                      action->getLastUpdate(), now - action->getLastUpdate());
    }

    action->finish();
    XBT_CDEBUG(surf_kernel, "Action %p finished", action);

    /* set the remains to 0 due to precision problems when updating the remaining amount */
    action->setRemains(0);
    action->setState(Action::State::done);
  }
  if (TRACE_is_enabled()) {
    //defining the last timestamp that we can safely dump to trace file
    //without losing the event ascending order (considering all CPU's)
    double smaller = -1;
    ActionList *actionSet = getRunningActionSet();
    ActionList::iterator it(actionSet->begin());
    ActionList::iterator itend(actionSet->end());
    for (; it != itend; ++it) {
      CpuAction *action = static_cast<CpuAction*>(&*it);
      if (smaller < 0 || action->getLastUpdate() < smaller)
        smaller = action->getLastUpdate();
    }
    if (smaller > 0) {
      TRACE_last_timestamp_to_dump = smaller;
    }
  }
}

void CpuModel::updateActionsStateFull(double now, double delta)
{
  CpuAction *action = nullptr;
  ActionList *running_actions = getRunningActionSet();
  ActionList::iterator it(running_actions->begin());
  ActionList::iterator itNext = it;
  ActionList::iterator itend(running_actions->end());
  for (; it != itend; it = itNext) {
    ++itNext;
    action = static_cast<CpuAction*>(&*it);
    if (TRACE_is_enabled()) {
      Cpu *cpu = static_cast<Cpu*> (lmm_constraint_id(lmm_get_cnst_from_var(getMaxminSystem(), action->getVariable(), 0)) );

      TRACE_surf_host_set_utilization(cpu->cname(), action->getCategory(), lmm_variable_getvalue(action->getVariable()),
                                      now - delta, delta);
      TRACE_last_timestamp_to_dump = now - delta;
    }

    action->updateRemains(lmm_variable_getvalue(action->getVariable()) * delta);

    if (action->getMaxDuration() != NO_MAX_DURATION)
      action->updateMaxDuration(delta);

    if (((action->getRemainsNoUpdate() <= 0) && (lmm_get_variable_weight(action->getVariable()) > 0)) ||
        ((action->getMaxDuration() != NO_MAX_DURATION) && (action->getMaxDuration() <= 0))) {
      action->finish();
      action->setState(Action::State::done);
    }
  }
}

/************
 * Resource *
 ************/
Cpu::Cpu(Model *model, simgrid::s4u::Host *host, std::vector<double> *speedPerPstate, int core)
 : Cpu(model, host, nullptr/*constraint*/, speedPerPstate, core)
{
}

Cpu::Cpu(Model* model, simgrid::s4u::Host* host, lmm_constraint_t constraint, std::vector<double>* speedPerPstate,
         int core)
    : Resource(model, host->getCname(), constraint), coresAmount_(core), host_(host)
{
  xbt_assert(core > 0, "Host %s must have at least one core, not 0.", host->getCname());

  speed_.peak = speedPerPstate->front();
  speed_.scale = 1;
  host->pimpl_cpu = this;
  xbt_assert(speed_.scale > 0, "Speed of host %s must be >0", host->getCname());

  // Copy the power peak array:
  for (double value : *speedPerPstate) {
    speedPerPstate_.push_back(value);
  }
}

Cpu::~Cpu() = default;

int Cpu::getNbPStates()
{
  return speedPerPstate_.size();
}

void Cpu::setPState(int pstate_index)
{
  xbt_assert(pstate_index <= static_cast<int>(speedPerPstate_.size()),
             "Invalid parameters for CPU %s (pstate %d > length of pstates %d)", cname(), pstate_index,
             static_cast<int>(speedPerPstate_.size()));

  double new_peak_speed = speedPerPstate_[pstate_index];
  pstate_ = pstate_index;
  speed_.peak = new_peak_speed;

  onSpeedChange();
}

int Cpu::getPState()
{
  return pstate_;
}

double Cpu::getPstateSpeed(int pstate_index)
{
  xbt_assert((pstate_index <= static_cast<int>(speedPerPstate_.size())), "Invalid parameters (pstate index out of bounds)");

  return speedPerPstate_[pstate_index];
}

double Cpu::getSpeed(double load)
{
  return load * speed_.peak;
}

double Cpu::getAvailableSpeed()
{
/* number between 0 and 1 */
  return speed_.scale;
}

void Cpu::onSpeedChange() {
  TRACE_surf_host_set_speed(surf_get_clock(), cname(), coresAmount_ * speed_.scale * speed_.peak);
  s4u::Host::onSpeedChange(*host_);
}

int Cpu::coreCount()
{
  return coresAmount_;
}

void Cpu::setStateTrace(tmgr_trace_t trace)
{
  xbt_assert(stateEvent_ == nullptr, "Cannot set a second state trace to Host %s", host_->getCname());

  stateEvent_ = future_evt_set->add_trace(trace, this);
}
void Cpu::setSpeedTrace(tmgr_trace_t trace)
{
  xbt_assert(speed_.event == nullptr, "Cannot set a second speed trace to Host %s", host_->getCname());

  speed_.event = future_evt_set->add_trace(trace, this);
}


/**********
 * Action *
 **********/

void CpuAction::updateRemainingLazy(double now)
{
  xbt_assert(getStateSet() == getModel()->getRunningActionSet(), "You're updating an action that is not running.");
  xbt_assert(getPriority() > 0, "You're updating an action that seems suspended.");

  double delta = now - lastUpdate_;

  if (remains_ > 0) {
    XBT_CDEBUG(surf_kernel, "Updating action(%p): remains was %f, last_update was: %f", this, remains_, lastUpdate_);
    double_update(&(remains_), lastValue_ * delta, sg_maxmin_precision*sg_surf_precision);

    if (TRACE_is_enabled()) {
      Cpu *cpu = static_cast<Cpu*>(lmm_constraint_id(lmm_get_cnst_from_var(getModel()->getMaxminSystem(), getVariable(), 0)));
      TRACE_surf_host_set_utilization(cpu->cname(), getCategory(), lastValue_, lastUpdate_, now - lastUpdate_);
    }
    XBT_CDEBUG(surf_kernel, "Updating action(%p): remains is now %f", this, remains_);
  }

  lastUpdate_ = now;
  lastValue_ = lmm_variable_getvalue(getVariable());
}

simgrid::xbt::signal<void(simgrid::surf::CpuAction*, Action::State)> CpuAction::onStateChange;

void CpuAction::setState(Action::State state){
  Action::State previous = getState();
  Action::setState(state);
  onStateChange(this, previous);
}
/** @brief returns a list of all CPUs that this action is using */
std::list<Cpu*> CpuAction::cpus() {
  std::list<Cpu*> retlist;
  lmm_system_t sys = getModel()->getMaxminSystem();
  int llen = lmm_get_number_of_cnst_from_var(sys, getVariable());

  for (int i = 0; i < llen; i++) {
    /* Beware of composite actions: ptasks put links and cpus together */
    // extra pb: we cannot dynamic_cast from void*...
    Resource* resource = static_cast<Resource*>(lmm_constraint_id(lmm_get_cnst_from_var(sys, getVariable(), i)));
    Cpu* cpu           = dynamic_cast<Cpu*>(resource);
    if (cpu != nullptr)
      retlist.push_back(cpu);
  }

  return retlist;
}

}
}
