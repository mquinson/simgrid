/* Copyright (c) 2004-2016. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "simgrid/s4u/VirtualMachine.hpp"
#include "src/simix/ActorImpl.hpp"
#include "src/surf/HostImpl.hpp"
#include <algorithm>
#include <deque>
#include <unordered_map>

#ifndef VM_INTERFACE_HPP_
#define VM_INTERFACE_HPP_

#define GUESTOS_NOISE 100 // This value corresponds to the cost of the global action associated to the VM
                          // It corresponds to the cost of a VM running no tasks.

typedef struct dirty_page* dirty_page_t;

namespace simgrid {
namespace vm {

/***********
 * Classes *
 ***********/

class XBT_PRIVATE VMModel;
XBT_PUBLIC_CLASS VirtualMachineImpl; // Made visible to the Java plugin

/*************
 * Callbacks *
 *************/

/** @ingroup SURF_callbacks
 * @brief Callbacks fired after VM creation. Signature: `void(VirtualMachine*)`
 */
extern XBT_PRIVATE simgrid::xbt::signal<void(simgrid::vm::VirtualMachineImpl*)> onVmCreation;

/** @ingroup SURF_callbacks
 * @brief Callbacks fired after VM destruction. Signature: `void(VirtualMachine*)`
 */
extern XBT_PRIVATE simgrid::xbt::signal<void(simgrid::vm::VirtualMachineImpl*)> onVmDestruction;

/** @ingroup SURF_callbacks
 * @brief Callbacks after VM State changes. Signature: `void(VirtualMachine*)`
 */
extern XBT_PRIVATE simgrid::xbt::signal<void(simgrid::vm::VirtualMachineImpl*)> onVmStateChange;

/************
 * Resource *
 ************/

/** @ingroup SURF_vm_interface
 * @brief SURF VM interface class
 * @details A VM represent a virtual machine
 */
XBT_PUBLIC_CLASS VirtualMachineImpl : public surf::HostImpl
{
  friend simgrid::s4u::VirtualMachine;

public:
  explicit VirtualMachineImpl(s4u::VirtualMachine * piface, s4u::Host * host, int coreAmount);
  ~VirtualMachineImpl();

  /** @brief Suspend the VM */
  virtual void suspend(simgrid::simix::ActorImpl* issuer);

  /** @brief Resume the VM */
  virtual void resume();

  /** @brief Shutdown the VM */
  virtual void shutdown(simgrid::simix::ActorImpl* issuer);

  /** @brief Change the physical host on which the given VM is running */
  virtual void setPm(s4u::Host* dest);

  /** @brief Get the physical machine hosting the VM */
  s4u::Host* getPm();

  sg_size_t getRamsize();

  virtual void setBound(double bound);

  void getParams(vm_params_t params);
  void setParams(vm_params_t params);

  /* The vm object of the lower layer */
  surf::Action* action_ = nullptr;

  /* Dirty pages stuff */
  std::unordered_map<std::string, dirty_page_t> dp_objs;
  int dp_enabled                     = 0;
  double dp_updated_by_deleted_tasks = 0;

protected:
  simgrid::s4u::Host* hostPM_;

public:
  e_surf_vm_state_t getState();
  void setState(e_surf_vm_state_t state);
  static std::deque<s4u::VirtualMachine*> allVms_;
  int coreAmount() { return coreAmount_; }

  bool isMigrating = false;

private:
  s_vm_params_t params_;
  int coreAmount_;

protected:
  e_surf_vm_state_t vmState_ = SURF_VM_STATE_CREATED;
};

/*********
 * Model *
 *********/
/** @ingroup SURF_vm_interface
 * @brief SURF VM model interface class
 * @details A model is an object which handle the interactions between its Resources and its Actions
 */
class VMModel : public surf::HostModel {
public:
  void ignoreEmptyVmInPmLMM() override{};

  double nextOccuringEvent(double now) override;
  void updateActionsState(double /*now*/, double /*delta*/) override{};
};
}
}

XBT_PUBLIC_DATA(simgrid::vm::VMModel*) surf_vm_model;

#endif /* VM_INTERFACE_HPP_ */
