/* Copyright (c) 2004-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <list>

#include <xbt/base.h>
#include <xbt/signal.hpp>

#include <simgrid/forward.h>
#include <simgrid/s4u/host.hpp>

#include "surf/datatypes.h"
#include "surf_interface.hpp"
#include "maxmin_private.hpp"
#include "trace_mgr.hpp"

#ifndef SURF_CPU_INTERFACE_HPP_
#define SURF_CPU_INTERFACE_HPP_

/***********
 * Classes *
 ***********/

namespace simgrid {
namespace surf {

class CpuModel;
class Cpu;
class CpuAction;
class CpuPlugin;// FIXME:DEADCODE

 /** @ingroup SURF_cpu_interface
 * @brief SURF cpu model interface class
 * @details A model is an object which handle the interactions between its Resources and its Actions
 */
XBT_PUBLIC_CLASS CpuModel : public Model {
public:
  CpuModel() : Model() {};
  ~CpuModel() override;

  /**
   * @brief Create a Cpu
   *
   * @param host The host that will have this CPU
   * @param speedPerPstate Processor speed (in Flops) of each pstate. This ignores any potential external load coming from a trace.
   * @param core The number of core of this Cpu
   */
  virtual Cpu *createCpu(simgrid::s4u::Host *host, std::vector<double> *speedPerPstate, int core)=0;

  void updateActionsStateLazy(double now, double delta) override;
  void updateActionsStateFull(double now, double delta) override;
  bool next_occuring_event_isIdempotent() override;
};

/************
 * Resource *
 ************/

/** @ingroup SURF_cpu_interface
* @brief SURF cpu resource interface class
* @details A Cpu represent a cpu associated to a host
*/
XBT_PUBLIC_CLASS Cpu : public simgrid::surf::Resource {
public:
  /**
   * @brief Cpu constructor
   *
   * @param model The CpuModel associated to this Cpu
   * @param host The host in which this Cpu should be plugged
   * @param constraint The lmm constraint associated to this Cpu if it is part of a LMM component
   * @param speedPerPstate Processor speed (in flop per second) for each pstate
   * @param core The number of core of this Cpu
   */
  Cpu(simgrid::surf::Model *model, simgrid::s4u::Host *host, lmm_constraint_t constraint,
      std::vector<double> *speedPerPstate, int core);

  /**
   * @brief Cpu constructor
   *
   * @param model The CpuModel associated to this Cpu
   * @param host The host in which this Cpu should be plugged
   * @param speedPerPstate Processor speed (in flop per second) for each pstate
   * @param core The number of core of this Cpu
   */
  Cpu(simgrid::surf::Model *model, simgrid::s4u::Host *host, std::vector<double> *speedPerPstate, int core);

  ~Cpu();

  /**
   * @brief Execute some quantity of computation
   *
   * @param size The value of the processing amount (in flop) needed to process
   * @return The CpuAction corresponding to the processing
   */
  virtual simgrid::surf::Action *execution_start(double size)=0;

  /**
   * @brief Make a process sleep for duration (in seconds)
   *
   * @param duration The number of seconds to sleep
   * @return The CpuAction corresponding to the sleeping
   */
  virtual simgrid::surf::Action *sleep(double duration)=0;

  /** @brief Get the amount of cores */
  virtual int getCoreCount();

  /** @brief Get the speed, accounting for the trace load and provided process load instead of the real current one */
  virtual double getSpeed(double load);

protected:
  /** @brief Take speed changes (either load or max) into account */
  virtual void onSpeedChange();

public:
  /** @brief Get the available speed of the current Cpu */
  virtual double getAvailableSpeed();

  /** @brief Get the current Cpu computational speed */
  virtual double getPstateSpeedCurrent();
  virtual double getPstateSpeed(int pstate_index);

  virtual int getNbPStates();
  virtual void setPState(int pstate_index);
  virtual int  getPState();

  simgrid::s4u::Host* getHost() { return host_; }

public:
  int coresAmount_ = 1;
  simgrid::s4u::Host* host_;

  std::vector<double> speedPerPstate_; /*< List of supported CPU capacities (pstate related) */
  int pstate_ = 0;                     /*< Current pstate (index in the speedPeakList)*/

public:
  virtual void setStateTrace(tmgr_trace_t trace); /*< setup the trace file with states events (ON or OFF). Trace must contain boolean values (0 or 1). */
  virtual void setSpeedTrace(tmgr_trace_t trace); /*< setup the trace file with availability events (peak speed changes due to external load). Trace must contain relative values (ratio between 0 and 1) */

  tmgr_trace_iterator_t stateEvent_ = nullptr;
  s_surf_metric_t speed_ = {1.0, 0, nullptr};
};

/**********
 * Action *
 **********/

 /** @ingroup SURF_cpu_interface
 * @brief A CpuAction represents the execution of code on one or several Cpus
 */
XBT_PUBLIC_CLASS CpuAction : public simgrid::surf::Action {
friend XBT_PUBLIC(Cpu*) getActionCpu(CpuAction *action);
public:
  /** @brief Callbacks handler which emit the callbacks after CpuAction State changed *
   * @details Callback functions have the following signature: `void(CpuAction *action, simgrid::surf::Action::State previous)`
   */
  static simgrid::xbt::signal<void(simgrid::surf::CpuAction*, simgrid::surf::Action::State)> onStateChange;

  CpuAction(simgrid::surf::Model *model, double cost, bool failed)
  : Action(model, cost, failed) {} //FIXME:DEADCODE?
  CpuAction(simgrid::surf::Model *model, double cost, bool failed, lmm_variable_t var)
  : Action(model, cost, failed, var) {}

  void setState(simgrid::surf::Action::State state) override;

  void updateRemainingLazy(double now) override;
  std::list<Cpu*> cpus();
};

}
}

#endif /* SURF_CPU_INTERFACE_HPP_ */
