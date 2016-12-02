/* Copyright (c) 2004-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "surf_interface.hpp"
#include "cpu_interface.hpp"
#include "mc/mc.h"
#include "network_interface.hpp"
#include "simgrid/s4u/engine.hpp"
#include "simgrid/sg_config.h"
#include "src/instr/instr_private.h" // TRACE_is_enabled(). FIXME: remove by subscribing tracing to the surf signals
#include "src/internal_config.h"
#include "src/simix/smx_host_private.h"
#include "src/surf/HostImpl.hpp"
#include "surf_private.h"
#include "surf_routing.hpp"
#include <vector>

XBT_LOG_NEW_CATEGORY(surf, "All SURF categories");
XBT_LOG_NEW_DEFAULT_SUBCATEGORY(surf_kernel, surf, "Logging specific to SURF (kernel)");

/*********
 * Utils *
 *********/

/* model_list_invoke contains only surf_host and surf_vm.
 * The callback functions of cpu_model and network_model will be called from those of these host models. */
std::vector<surf_model_t> * all_existing_models = nullptr; /* to destroy models correctly */
xbt_dynar_t model_list_invoke = nullptr;  /* to invoke callbacks */

simgrid::trace_mgr::future_evt_set *future_evt_set = nullptr;
xbt_dynar_t surf_path = nullptr;
std::vector<simgrid::s4u::Host*> host_that_restart;
xbt_dict_t watched_hosts_lib;

namespace simgrid {
namespace surf {

simgrid::xbt::signal<void(void)> surfExitCallbacks;

}
}

#include <simgrid/plugins/energy.h> // FIXME: this plugin should not be linked to the core

s_surf_model_description_t surf_plugin_description[] = {
    {"Energy", "Cpu energy consumption.", &sg_energy_plugin_init},
     {nullptr, nullptr,  nullptr}      /* this array must be nullptr terminated */
};

/* Don't forget to update the option description in smx_config when you change this */
s_surf_model_description_t surf_network_model_description[] = {
  {"LV08", "Realistic network analytic model (slow-start modeled by multiplying latency by 10.4, bandwidth by .92; bottleneck sharing uses a payload of S=8775 for evaluating RTT). ",
   &surf_network_model_init_LegrandVelho},
  {"Constant",
   "Simplistic network model where all communication take a constant time (one second). This model provides the lowest realism, but is (marginally) faster.",
   &surf_network_model_init_Constant},
  {"SMPI", "Realistic network model specifically tailored for HPC settings (accurate modeling of slow start with correction factors on three intervals: < 1KiB, < 64 KiB, >= 64 KiB)",
   &surf_network_model_init_SMPI},
  {"IB", "Realistic network model specifically tailored for HPC settings, with Infiniband contention model",
   &surf_network_model_init_IB},
  {"CM02", "Legacy network analytic model (Very similar to LV08, but without corrective factors. The timings of small messages are thus poorly modeled).",
   &surf_network_model_init_CM02},
  {"NS3", "Network pseudo-model using the NS3 tcp model instead of an analytic model", &surf_network_model_init_NS3},
  {"Reno",  "Model from Steven H. Low using lagrange_solve instead of lmm_solve (experts only; check the code for more info).",
   &surf_network_model_init_Reno},
  {"Reno2", "Model from Steven H. Low using lagrange_solve instead of lmm_solve (experts only; check the code for more info).",
   &surf_network_model_init_Reno2},
  {"Vegas", "Model from Steven H. Low using lagrange_solve instead of lmm_solve (experts only; check the code for more info).",
   &surf_network_model_init_Vegas},
  {nullptr, nullptr, nullptr}      /* this array must be nullptr terminated */
};

#if ! HAVE_SMPI
void surf_network_model_init_SMPI() {
  xbt_die("Please activate SMPI support in cmake to use the SMPI network model.");
}
void surf_network_model_init_IB() {
  xbt_die("Please activate SMPI support in cmake to use the IB network model.");
}
#endif
#if !HAVE_NS3
void surf_network_model_init_NS3() {
  xbt_die("Please activate NS3 support in cmake and install the dependencies to use the NS3 network model.");
}
#endif

s_surf_model_description_t surf_cpu_model_description[] = {
  {"Cas01", "Simplistic CPU model (time=size/power).", &surf_cpu_model_init_Cas01},
  {nullptr, nullptr,  nullptr}      /* this array must be nullptr terminated */
};

s_surf_model_description_t surf_host_model_description[] = {
  {"default",   "Default host model. Currently, CPU:Cas01 and network:LV08 (with cross traffic enabled)", &surf_host_model_init_current_default},
  {"compound",  "Host model that is automatically chosen if you change the network and CPU models", &surf_host_model_init_compound},
  {"ptask_L07", "Host model somehow similar to Cas01+CM02 but allowing parallel tasks", &surf_host_model_init_ptask_L07},
  {nullptr, nullptr, nullptr}      /* this array must be nullptr terminated */
};

s_surf_model_description_t surf_vm_model_description[] = {
  {"default", "Default vm model.", &surf_vm_model_init_HL13},
  {nullptr, nullptr, nullptr}      /* this array must be nullptr terminated */
};

s_surf_model_description_t surf_optimization_mode_description[] = {
  {"Lazy", "Lazy action management (partial invalidation in lmm + heap in action remaining).", nullptr},
  {"TI",   "Trace integration. Highly optimized mode when using availability traces (only available for the Cas01 CPU model for now).", nullptr},
  {"Full", "Full update of remaining and variables. Slow but may be useful when debugging.", nullptr},
  {nullptr, nullptr, nullptr}      /* this array must be nullptr terminated */
};

s_surf_model_description_t surf_storage_model_description[] = {
  {"default", "Simplistic storage model.", &surf_storage_model_init_default},
  {nullptr, nullptr,  nullptr}      /* this array must be nullptr terminated */
};

#if HAVE_THREAD_CONTEXTS
static xbt_parmap_t surf_parmap = nullptr; /* parallel map on models */
#endif

double NOW = 0;

double surf_get_clock()
{
  return NOW;
}

#ifdef _WIN32
# define FILE_DELIM "\\"
#else
# define FILE_DELIM "/"         /* FIXME: move to better location */
#endif

FILE *surf_fopen(const char *name, const char *mode)
{
  unsigned int cpt;
  char *path_elm = nullptr;
  char *buff;
  FILE *file = nullptr;

  xbt_assert(name);

  if (__surf_is_absolute_file_path(name))       /* don't mess with absolute file names */
    return fopen(name, mode);

  /* search relative files in the path */
  xbt_dynar_foreach(surf_path, cpt, path_elm) {
    buff = bprintf("%s" FILE_DELIM "%s", path_elm, name);
    file = fopen(buff, mode);
    free(buff);

    if (file)
      return file;
  }
  return nullptr;
}

#ifdef _WIN32
#include <windows.h>
#define MAX_DRIVE 26
static const char *disk_drives_letter_table[MAX_DRIVE] = {
  "A:\\","B:\\","C:\\","D:\\","E:\\","F:\\","G:\\","H:\\","I:\\","J:\\","K:\\","L:\\","M:\\",
  "N:\\","O:\\","P:\\","Q:\\","R:\\","S:\\","T:\\","U:\\","V:\\","W:\\","X:\\","Y:\\","Z:\\"
};
#endif

/*
 * Returns the initial path. On Windows the initial path is
 * the current directory for the current process in the other
 * case the function returns "./" that represents the current
 * directory on Unix/Linux platforms.
 */

const char *__surf_get_initial_path()
{

#ifdef _WIN32
  unsigned i;
  char current_directory[MAX_PATH + 1] = { 0 };
  unsigned int len = GetCurrentDirectory(MAX_PATH + 1, current_directory);
  char root[4] = { 0 };

  if (!len)
    return nullptr;

  strncpy(root, current_directory, 3);

  for (i = 0; i < MAX_DRIVE; i++) {
    if (toupper(root[0]) == disk_drives_letter_table[i][0])
      return disk_drives_letter_table[i];
  }

  return nullptr;
#else
  return "./";
#endif
}

/* The __surf_is_absolute_file_path() returns 1 if
 * file_path is a absolute file path, in the other
 * case the function returns 0.
 */
int __surf_is_absolute_file_path(const char *file_path)
{
#ifdef _WIN32
  WIN32_FIND_DATA wfd = { 0 };
  HANDLE hFile = FindFirstFile(file_path, &wfd);

  if (INVALID_HANDLE_VALUE == hFile)
    return 0;

  FindClose(hFile);
  return 1;
#else
  return (file_path[0] == '/');
#endif
}

/** Displays the long description of all registered models, and quit */
void model_help(const char *category, s_surf_model_description_t * table)
{
  printf("Long description of the %s models accepted by this simulator:\n", category);
  for (int i = 0; table[i].name; i++)
    printf("  %s: %s\n", table[i].name, table[i].description);
}

int find_model_description(s_surf_model_description_t * table,
                           const char *name)
{
  int i;
  char *name_list = nullptr;

  for (i = 0; table[i].name; i++)
    if (!strcmp(name, table[i].name)) {
      return i;
    }
  if (!table[0].name)
    xbt_die("No model is valid! This is a bug.");
  name_list = xbt_strdup(table[0].name);
  for (i = 1; table[i].name; i++) {
    name_list = (char *) xbt_realloc(name_list, strlen(name_list) + strlen(table[i].name) + 3);
    strncat(name_list, ", ", 2);
    strncat(name_list, table[i].name, strlen(table[i].name));
  }
  xbt_die("Model '%s' is invalid! Valid models are: %s.", name, name_list);
  return -1;
}

static inline void surf_storage_free(void *r)
{
  delete static_cast<simgrid::surf::Storage*>(r);
}

void sg_version_check(int lib_version_major,int lib_version_minor,int lib_version_patch) {
    if ((lib_version_major != SIMGRID_VERSION_MAJOR) || (lib_version_minor != SIMGRID_VERSION_MINOR)) {
      fprintf(stderr,
          "FATAL ERROR: Your program was compiled with SimGrid version %d.%d.%d, "
          "and then linked against SimGrid %d.%d.%d. Please fix this.\n",
          lib_version_major,lib_version_minor,lib_version_patch,
          SIMGRID_VERSION_MAJOR,SIMGRID_VERSION_MINOR,SIMGRID_VERSION_PATCH);
      abort();
    }
    if (lib_version_patch != SIMGRID_VERSION_PATCH) {
      if(SIMGRID_VERSION_PATCH >= 90 || lib_version_patch >=90){
        fprintf(stderr,
        "FATAL ERROR: Your program was compiled with SimGrid version %d.%d.%d, "
        "and then linked against SimGrid %d.%d.%d. \n"
        "One of them is a development version, and should not be mixed with the stable release. Please fix this.\n",
        lib_version_major,lib_version_minor,lib_version_patch,
        SIMGRID_VERSION_MAJOR,SIMGRID_VERSION_MINOR,SIMGRID_VERSION_PATCH);
        abort();
      }
        fprintf(stderr,
            "Warning: Your program was compiled with SimGrid version %d.%d.%d, "
            "and then linked against SimGrid %d.%d.%d. Proceeding anyway.\n",
            lib_version_major,lib_version_minor,lib_version_patch,
            SIMGRID_VERSION_MAJOR,SIMGRID_VERSION_MINOR,SIMGRID_VERSION_PATCH);
    }
}

void sg_version(int *ver_major,int *ver_minor,int *ver_patch) {
  *ver_major = SIMGRID_VERSION_MAJOR;
  *ver_minor = SIMGRID_VERSION_MINOR;
  *ver_patch = SIMGRID_VERSION_PATCH;
}

void surf_init(int *argc, char **argv)
{
  if (USER_HOST_LEVEL != -1) // Already initialized
    return;

  XBT_DEBUG("Create all Libs");
  USER_HOST_LEVEL = simgrid::s4u::Host::extension_create(nullptr);

  as_router_lib = xbt_lib_new();
  storage_lib = xbt_lib_new();
  storage_type_lib = xbt_lib_new();
  file_lib = xbt_lib_new();
  watched_hosts_lib = xbt_dict_new_homogeneous(nullptr);

  XBT_DEBUG("Add routing levels");
  ROUTING_PROP_ASR_LEVEL = xbt_lib_add_level(as_router_lib, nullptr);
  ROUTING_ASR_LEVEL = xbt_lib_add_level(as_router_lib, [](void* p) {
    delete static_cast<simgrid::kernel::routing::NetCard*>(p);
  });

  XBT_DEBUG("Add SURF levels");
  SURF_STORAGE_LEVEL = xbt_lib_add_level(storage_lib,surf_storage_free);

  xbt_init(argc, argv);
  if (!all_existing_models)
    all_existing_models = new std::vector<simgrid::surf::Model*>();
  if (!model_list_invoke)
    model_list_invoke = xbt_dynar_new(sizeof(simgrid::surf::Model*), nullptr);
  if (!future_evt_set)
    future_evt_set = new simgrid::trace_mgr::future_evt_set();

  TRACE_add_start_function(TRACE_surf_alloc);
  TRACE_add_end_function(TRACE_surf_release);

  sg_config_init(argc, argv);

  if (MC_is_active())
    MC_memory_init();
}

void surf_exit()
{
  TRACE_end();                  /* Just in case it was not called by the upper layer (or there is no upper layer) */

  xbt_dynar_free(&surf_path);

  sg_host_exit();
  xbt_lib_free(&as_router_lib);
  xbt_lib_free(&storage_lib);
  sg_link_exit();
  xbt_lib_free(&storage_type_lib);
  xbt_lib_free(&file_lib);
  xbt_dict_free(&watched_hosts_lib);

  for (auto model : *all_existing_models)
    delete model;
  delete all_existing_models;
  xbt_dynar_free(&model_list_invoke);
  routing_exit();

  simgrid::surf::surfExitCallbacks();

  if (future_evt_set) {
    delete future_evt_set;
    future_evt_set = nullptr;
  }

#if HAVE_THREAD_CONTEXTS
  xbt_parmap_destroy(surf_parmap);
#endif

  tmgr_finalize();
  sg_platf_exit();
  simgrid::s4u::Engine::shutdown();

  NOW = 0;                      /* Just in case the user plans to restart the simulation afterward */
}

/*********
 * Model *
 *********/

namespace simgrid {
namespace surf {

Model::Model()
  : maxminSystem_(nullptr)
{
  readyActionSet_ = new ActionList();
  runningActionSet_ = new ActionList();
  failedActionSet_ = new ActionList();
  doneActionSet_ = new ActionList();

  modifiedSet_ = nullptr;
  actionHeap_ = nullptr;
  updateMechanism_ = UM_UNDEFINED;
  selectiveUpdate_ = 0;
}

Model::~Model(){
  delete readyActionSet_;
  delete runningActionSet_;
  delete failedActionSet_;
  delete doneActionSet_;
}

double Model::nextOccuringEvent(double now)
{
  //FIXME: set the good function once and for all
  if (updateMechanism_ == UM_LAZY)
    return nextOccuringEventLazy(now);
  else if (updateMechanism_ == UM_FULL)
    return nextOccuringEventFull(now);
  else
    xbt_die("Invalid cpu update mechanism!");
}

double Model::nextOccuringEventLazy(double now)
{
  XBT_DEBUG("Before share resources, the size of modified actions set is %zd", modifiedSet_->size());
  lmm_solve(maxminSystem_);
  XBT_DEBUG("After share resources, The size of modified actions set is %zd", modifiedSet_->size());

  while(!modifiedSet_->empty()) {
    Action *action = &(modifiedSet_->front());
    modifiedSet_->pop_front();
    int max_dur_flag = 0;

    if (action->getStateSet() != runningActionSet_)
      continue;

    /* bogus priority, skip it */
    if (action->getPriority() <= 0 || action->getHat()==LATENCY)
      continue;

    action->updateRemainingLazy(now);

    double min = -1;
    double share = lmm_variable_getvalue(action->getVariable());

    if (share > 0) {
      double time_to_completion;
      if (action->getRemains() > 0) {
        time_to_completion = action->getRemainsNoUpdate() / share;
      } else {
        time_to_completion = 0.0;
      }
      min = now + time_to_completion; // when the task will complete if nothing changes
    }

    if ((action->getMaxDuration() != NO_MAX_DURATION)
        && (min == -1
            || action->getStartTime() +
            action->getMaxDuration() < min)) {
      min = action->getStartTime() +
          action->getMaxDuration();  // when the task will complete anyway because of the deadline if any
      max_dur_flag = 1;
    }


    XBT_DEBUG("Action(%p) corresponds to variable %d", action, action->getVariable()->id_int);

    XBT_DEBUG("Action(%p) Start %f. May finish at %f (got a share of %f). Max_duration %f", action,
        action->getStartTime(), min, share,
        action->getMaxDuration());

    if (min != -1) {
      action->heapUpdate(actionHeap_, min, max_dur_flag ? MAX_DURATION : NORMAL);
      XBT_DEBUG("Insert at heap action(%p) min %f now %f", action, min,
                now);
    } else DIE_IMPOSSIBLE;
  }

  //hereafter must have already the min value for this resource model
  if (xbt_heap_size(actionHeap_) > 0) {
    double min = xbt_heap_maxkey(actionHeap_) - now;
    XBT_DEBUG("minimum with the HEAP %f", min);
    return min;
  } else {
    XBT_DEBUG("The HEAP is empty, thus returning -1");
    return -1;
  }
}

double Model::nextOccuringEventFull(double /*now*/) {
  maxminSystem_->solve_fun(maxminSystem_);

  double min = -1;
  for (auto it(getRunningActionSet()->begin()), itend(getRunningActionSet()->end()); it != itend ; ++it) {
    Action *action = &*it;
    double value = lmm_variable_getvalue(action->getVariable());
    if (value > 0) {
      if (action->getRemains() > 0)
        value = action->getRemainsNoUpdate() / value;
      else
        value = 0.0;
      if (min < 0 || value < min) {
        min = value;
        XBT_DEBUG("Updating min (value) with %p: %f", action, min);
      }
    }
    if ((action->getMaxDuration() >= 0) && (min<0 || action->getMaxDuration() < min)) {
      min = action->getMaxDuration();
      XBT_DEBUG("Updating min (duration) with %p: %f", action, min);
    }
  }
  XBT_DEBUG("min value : %f", min);

  return min;
}

void Model::updateActionsState(double now, double delta)
{
  if (updateMechanism_ == UM_FULL)
    updateActionsStateFull(now, delta);
  else if (updateMechanism_ == UM_LAZY)
    updateActionsStateLazy(now, delta);
  else
    xbt_die("Invalid cpu update mechanism!");
}

void Model::updateActionsStateLazy(double /*now*/, double /*delta*/)
{
  THROW_UNIMPLEMENTED;
}

void Model::updateActionsStateFull(double /*now*/, double /*delta*/)
{
  THROW_UNIMPLEMENTED;
}

}
}

/************
 * Resource *
 ************/

namespace simgrid {
namespace surf {

Resource::Resource(Model* model, const char* name, lmm_constraint_t constraint)
    : name_(name), model_(model), constraint_(constraint)
{}

Resource::~Resource() = default;

bool Resource::isOn() const {
  return isOn_;
}
bool Resource::isOff() const {
  return ! isOn_;
}

void Resource::turnOn()
{
  isOn_ = true;
}

void Resource::turnOff()
{
  isOn_ = false;
}

Model *Resource::getModel() const {
  return model_;
}

const char *Resource::getName() const {
  return name_.c_str();
}

bool Resource::operator==(const Resource &other) const {
  return name_ == other.name_;
}

lmm_constraint_t Resource::getConstraint() const {
  return constraint_;
}

}
}

/**********
 * Action *
 **********/

const char *surf_action_state_names[6] = {
  "SURF_ACTION_READY",
  "SURF_ACTION_RUNNING",
  "SURF_ACTION_FAILED",
  "SURF_ACTION_DONE",
  "SURF_ACTION_TO_FREE",
  "SURF_ACTION_NOT_IN_THE_SYSTEM"
};

/* added to manage the communication action's heap */
void surf_action_lmm_update_index_heap(void *action, int i) {
  static_cast<simgrid::surf::Action*>(action)->updateIndexHeap(i);
}

namespace simgrid {
namespace surf {

Action::Action(simgrid::surf::Model* model, double cost, bool failed) : Action(model, cost, failed, nullptr)
{
}

Action::Action(simgrid::surf::Model* model, double cost, bool failed, lmm_variable_t var)
    : remains_(cost), start_(surf_get_clock()), cost_(cost), model_(model), variable_(var)
{
  if (failed)
    stateSet_ = getModel()->getFailedActionSet();
  else
    stateSet_ = getModel()->getRunningActionSet();

  stateSet_->push_back(*this);
}

Action::~Action() {
  xbt_free(category_);
}

void Action::finish() {
    finishTime_ = surf_get_clock();
}

Action::State Action::getState()
{
  if (stateSet_ ==  getModel()->getReadyActionSet())
    return Action::State::ready;
  if (stateSet_ ==  getModel()->getRunningActionSet())
    return Action::State::running;
  if (stateSet_ ==  getModel()->getFailedActionSet())
    return Action::State::failed;
  if (stateSet_ ==  getModel()->getDoneActionSet())
    return Action::State::done;
  return Action::State::not_in_the_system;
}

void Action::setState(Action::State state)
{
  stateSet_->erase(stateSet_->iterator_to(*this));
  switch (state) {
  case Action::State::ready:
    stateSet_ = getModel()->getReadyActionSet();
    break;
  case Action::State::running:
    stateSet_ = getModel()->getRunningActionSet();
    break;
  case Action::State::failed:
    stateSet_ = getModel()->getFailedActionSet();
    break;
  case Action::State::done:
    stateSet_ = getModel()->getDoneActionSet();
    break;
  default:
    stateSet_ = nullptr;
    break;
  }
  if (stateSet_)
    stateSet_->push_back(*this);
}

double Action::getBound()
{
  return (variable_) ? lmm_variable_getbound(variable_) : 0;
}

void Action::setBound(double bound)
{
  XBT_IN("(%p,%g)", this, bound);
  if (variable_)
    lmm_update_variable_bound(getModel()->getMaxminSystem(), variable_, bound);

  if (getModel()->getUpdateMechanism() == UM_LAZY && getLastUpdate()!=surf_get_clock())
    heapRemove(getModel()->getActionHeap());
  XBT_OUT();
}

double Action::getStartTime()
{
  return start_;
}

double Action::getFinishTime()
{
  /* keep the function behavior, some models (cpu_ti) change the finish time before the action end */
  return remains_ == 0 ? finishTime_ : -1;
}

void Action::setData(void* data)
{
  data_ = data;
}

void Action::setCategory(const char *category)
{
  category_ = xbt_strdup(category);
}

void Action::ref(){
  refcount_++;
}

void Action::setMaxDuration(double duration)
{
  maxDuration_ = duration;
  if (getModel()->getUpdateMechanism() == UM_LAZY)      // remove action from the heap
    heapRemove(getModel()->getActionHeap());
}

void Action::gapRemove() {}

void Action::setPriority(double priority)
{
  XBT_IN("(%p,%g)", this, priority);
  priority_ = priority;
  lmm_update_variable_weight(getModel()->getMaxminSystem(), getVariable(), priority);

  if (getModel()->getUpdateMechanism() == UM_LAZY)
    heapRemove(getModel()->getActionHeap());
  XBT_OUT();
}

void Action::cancel(){
  setState(Action::State::failed);
  if (getModel()->getUpdateMechanism() == UM_LAZY) {
    if (action_lmm_hook.is_linked())
      getModel()->getModifiedSet()->erase(getModel()->getModifiedSet()->iterator_to(*this));
    heapRemove(getModel()->getActionHeap());
  }
}

int Action::unref(){
  refcount_--;
  if (!refcount_) {
    if (action_hook.is_linked())
      stateSet_->erase(stateSet_->iterator_to(*this));
    if (getVariable())
      lmm_variable_free(getModel()->getMaxminSystem(), getVariable());
    if (getModel()->getUpdateMechanism() == UM_LAZY) {
      /* remove from heap */
      heapRemove(getModel()->getActionHeap());
      if (action_lmm_hook.is_linked())
        getModel()->getModifiedSet()->erase(getModel()->getModifiedSet()->iterator_to(*this));
    }
    delete this;
    return 1;
  }
  return 0;
}

void Action::suspend()
{
  XBT_IN("(%p)", this);
  if (suspended_ != 2) {
    lmm_update_variable_weight(getModel()->getMaxminSystem(), getVariable(), 0.0);
    suspended_ = 1;
    if (getModel()->getUpdateMechanism() == UM_LAZY)
      heapRemove(getModel()->getActionHeap());
  }
  XBT_OUT();
}

void Action::resume()
{
  XBT_IN("(%p)", this);
  if (suspended_ != 2) {
    lmm_update_variable_weight(getModel()->getMaxminSystem(), getVariable(), priority_);
    suspended_ = 0;
    if (getModel()->getUpdateMechanism() == UM_LAZY)
      heapRemove(getModel()->getActionHeap());
  }
  XBT_OUT();
}

bool Action::isSuspended()
{
  return suspended_ == 1;
}
/* insert action on heap using a given key and a hat (heap_action_type)
 * a hat can be of three types for communications:
 *
 * NORMAL = this is a normal heap entry stating the date to finish transmitting
 * LATENCY = this is a heap entry to warn us when the latency is payed
 * MAX_DURATION =this is a heap entry to warn us when the max_duration limit is reached
 */
void Action::heapInsert(xbt_heap_t heap, double key, enum heap_action_type hat)
{
  hat_ = hat;
  xbt_heap_push(heap, this, key);
}

void Action::heapRemove(xbt_heap_t heap)
{
  hat_ = NOTSET;
  if (indexHeap_ >= 0) {
    xbt_heap_remove(heap, indexHeap_);
  }
}

void Action::heapUpdate(xbt_heap_t heap, double key, enum heap_action_type hat)
{
  hat_ = hat;
  if (indexHeap_ >= 0) {
    xbt_heap_update(heap, indexHeap_, key);
  }else{
    xbt_heap_push(heap, this, key);
  }
}

void Action::updateIndexHeap(int i) {
  indexHeap_ = i;
}

double Action::getRemains()
{
  XBT_IN("(%p)", this);
  /* update remains before return it */
  if (getModel()->getUpdateMechanism() == UM_LAZY)      /* update remains before return it */
    updateRemainingLazy(surf_get_clock());
  XBT_OUT();
  return remains_;
}

double Action::getRemainsNoUpdate()
{
  return remains_;
}

//FIXME split code in the right places
void Action::updateRemainingLazy(double now)
{
  double delta = 0.0;

  if(getModel() == surf_network_model)
  {
    if (suspended_ != 0)
      return;
  }
  else
  {
    xbt_assert(stateSet_ == getModel()->getRunningActionSet(), "You're updating an action that is not running.");
    xbt_assert(priority_ > 0, "You're updating an action that seems suspended.");
  }

  delta = now - lastUpdate_;

  if (remains_ > 0) {
    XBT_DEBUG("Updating action(%p): remains was %f, last_update was: %f", this, remains_, lastUpdate_);
    double_update(&remains_, lastValue_ * delta, sg_surf_precision*sg_maxmin_precision);

    if (getModel() == surf_cpu_model_pm && TRACE_is_enabled()) {
      simgrid::surf::Resource *cpu = static_cast<simgrid::surf::Resource*>(
        lmm_constraint_id(lmm_get_cnst_from_var(getModel()->getMaxminSystem(), getVariable(), 0)));
      TRACE_surf_host_set_utilization(cpu->getName(), getCategory(), lastValue_, lastUpdate_, now - lastUpdate_);
    }
    XBT_DEBUG("Updating action(%p): remains is now %f", this, remains_);
  }

  if(getModel() == surf_network_model)
  {
    if (maxDuration_ != NO_MAX_DURATION)
      double_update(&maxDuration_, delta, sg_surf_precision);

    //FIXME: duplicated code
    if ((remains_ <= 0) &&
        (lmm_get_variable_weight(getVariable()) > 0)) {
      finish();
      setState(Action::State::done);
      heapRemove(getModel()->getActionHeap());
    } else if (((maxDuration_ != NO_MAX_DURATION)
        && (maxDuration_ <= 0))) {
      finish();
      setState(Action::State::done);
      heapRemove(getModel()->getActionHeap());
    }
  }

  lastUpdate_ = now;
  lastValue_ = lmm_variable_getvalue(getVariable());
}

}
}
