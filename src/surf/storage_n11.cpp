/* Copyright (c) 2013-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "storage_n11.hpp"
#include "surf_private.h"
#include <math.h> /*ceil*/
XBT_LOG_EXTERNAL_DEFAULT_CATEGORY(surf_storage);

static int storage_selective_update = 0;
static xbt_swag_t storage_running_action_set_that_does_not_need_being_checked = nullptr;

/*************
 * CallBacks *
 *************/

static inline void routing_storage_type_free(void *r)
{
  storage_type_t stype = (storage_type_t) r;
  free(stype->model);
  free(stype->type_id);
  free(stype->content);
  free(stype->content_type);
  xbt_dict_free(&(stype->properties));
  xbt_dict_free(&(stype->model_properties));
  free(stype);
}

static inline void surf_storage_resource_free(void *r)
{
  // specific to storage
  simgrid::surf::Storage *storage = static_cast<simgrid::surf::Storage*>(r);
  // generic resource
  delete storage;
}

static inline void routing_storage_host_free(void *r)
{
  xbt_dynar_t dyn = (xbt_dynar_t) r;
  xbt_dynar_free(&dyn);
}

void storage_register_callbacks()
{
  ROUTING_STORAGE_LEVEL = xbt_lib_add_level(storage_lib, xbt_free_f);
  ROUTING_STORAGE_HOST_LEVEL = xbt_lib_add_level(storage_lib, routing_storage_host_free);
  ROUTING_STORAGE_TYPE_LEVEL = xbt_lib_add_level(storage_type_lib, routing_storage_type_free);
  SURF_STORAGE_LEVEL = xbt_lib_add_level(storage_lib, surf_storage_resource_free);
}

/*********
 * Model *
 *********/

void surf_storage_model_init_default()
{
  surf_storage_model = new simgrid::surf::StorageN11Model();
  all_existing_models->push_back(surf_storage_model);
}

namespace simgrid {
namespace surf {

StorageN11Model::StorageN11Model() : StorageModel() {
  Action *action = nullptr;

  XBT_DEBUG("surf_storage_model_init_internal");

  storage_running_action_set_that_does_not_need_being_checked = xbt_swag_new(xbt_swag_offset(*action, stateHookup_));
  if (!maxminSystem_) {
    maxminSystem_ = lmm_system_new(storage_selective_update);
  }
}

StorageN11Model::~StorageN11Model(){
  xbt_swag_free(storage_running_action_set_that_does_not_need_being_checked);
  storage_running_action_set_that_does_not_need_being_checked = nullptr;
}

#include "src/surf/xml/platf.hpp" // FIXME: move that back to the parsing area
Storage *StorageN11Model::createStorage(const char* id, const char* type_id,
    const char* content_name, const char* content_type, xbt_dict_t properties, const char* attach)
{

  xbt_assert(!surf_storage_resource_priv(surf_storage_resource_by_name(id)),
      "Storage '%s' declared several times in the platform file", id);

  storage_type_t storage_type = (storage_type_t) xbt_lib_get_or_null(storage_type_lib, type_id,ROUTING_STORAGE_TYPE_LEVEL);

  double Bread  = surf_parse_get_bandwidth((char*)xbt_dict_get(storage_type->model_properties, "Bread"),
      "property Bread, storage",type_id);
  double Bwrite = surf_parse_get_bandwidth((char*)xbt_dict_get(storage_type->model_properties, "Bwrite"),
      "property Bwrite, storage",type_id);
  double Bconnection   = surf_parse_get_bandwidth((char*)xbt_dict_get(storage_type->model_properties, "Bconnection"),
      "property Bconnection, storage",type_id);

  Storage *storage = new StorageN11(this, id, properties, maxminSystem_,
      Bread, Bwrite, Bconnection, type_id, (char *)content_name,
      content_type, storage_type->size, (char *) attach);
  storageCreatedCallbacks(storage);
  xbt_lib_set(storage_lib, id, SURF_STORAGE_LEVEL, storage);

  XBT_DEBUG("SURF storage create resource\n\t\tid '%s'\n\t\ttype '%s'\n\t\tproperties '%p'\n\t\tBread '%f'\n",
      id, type_id, properties, Bread);

  p_storageList.push_back(storage);

  return storage;
}

double StorageN11Model::next_occuring_event(double /*now*/)
{
  XBT_DEBUG("storage_share_resources");

  double min_completion = shareResourcesMaxMin(getRunningActionSet(), maxminSystem_, lmm_solve);

  double rate;
  // Foreach disk
  for(auto storage: p_storageList) {
    rate = 0;
    // Foreach write action on disk
    for (auto write_action: storage->writeActions_) {
      rate += lmm_variable_getvalue(write_action->getVariable());
    }
    if(rate > 0)
      min_completion = MIN(min_completion, (storage->size_-storage->usedSize_)/rate);
  }

  return min_completion;
}

void StorageN11Model::updateActionsState(double /*now*/, double delta)
{
  StorageAction *action = nullptr;

  ActionList *actionSet = getRunningActionSet();
  for(ActionList::iterator it(actionSet->begin()), itNext=it, itend(actionSet->end())
      ; it != itend ; it=itNext) {
    ++itNext;
    action = static_cast<StorageAction*>(&*it);

    if(action->m_type == WRITE){
      // Update the disk usage
      // Update the file size
      // For each action of type write
      double current_progress =
          delta * lmm_variable_getvalue(action->getVariable());
      long int incr = current_progress;

      XBT_DEBUG("%s:\n\t progress =  %.2f, current_progress = %.2f, incr = %ld, lrint(1) = %ld, lrint(2) = %ld",
          action->p_file->name,
          action->progress,  current_progress, incr,
          lrint(action->progress + current_progress),
          lrint(action->progress)+ incr);

      /* take care of rounding error accumulation */
      if (lrint(action->progress + current_progress) > lrint(action->progress)+ incr)
        incr++;

      action->progress +=current_progress;

      action->p_storage->usedSize_ += incr; // disk usage
      action->p_file->current_position+= incr; // current_position
      //  which becomes the new file size
      action->p_file->size = action->p_file->current_position ;

      sg_size_t *psize = xbt_new(sg_size_t,1);
      *psize = action->p_file->size;
      xbt_dict_t content_dict = action->p_storage->content_;
      xbt_dict_set(content_dict, action->p_file->name, psize, nullptr);
    }

    action->updateRemains(lmm_variable_getvalue(action->getVariable()) * delta);

    if (action->getMaxDuration() > NO_MAX_DURATION)
      action->updateMaxDuration(delta);

    if(action->getRemainsNoUpdate() > 0 && lmm_get_variable_weight(action->getVariable()) > 0 &&
        action->p_storage->usedSize_ == action->p_storage->size_) {
      action->finish();
      action->setState(Action::State::failed);
    } else if (((action->getRemainsNoUpdate() <= 0) && (lmm_get_variable_weight(action->getVariable()) > 0)) ||
               ((action->getMaxDuration() > NO_MAX_DURATION) && (action->getMaxDuration() <= 0))) {
      action->finish();
      action->setState(Action::State::done);
    }
  }
  return;
}

/************
 * Resource *
 ************/

StorageN11::StorageN11(StorageModel *model, const char* name,
    xbt_dict_t properties, lmm_system_t maxminSystem, double bread,
    double bwrite, double bconnection, const char* type_id, char *content_name,
    const char *content_type, sg_size_t size, char *attach)
: Storage(model, name, properties,
    maxminSystem, bread, bwrite, bconnection, type_id, content_name, content_type, size, attach) {
  XBT_DEBUG("Create resource with Bconnection '%f' Bread '%f' Bwrite '%f' and Size '%llu'", bconnection, bread, bwrite, size);
}

StorageAction *StorageN11::open(const char* mount, const char* path)
{
  XBT_DEBUG("\tOpen file '%s'",path);

  sg_size_t size, *psize;
  psize = (sg_size_t*) xbt_dict_get_or_null(content_, path);
  // if file does not exist create an empty file
  if(psize)
    size = *psize;
  else {
    psize = xbt_new(sg_size_t,1);
    size = 0;
    *psize = size;
    xbt_dict_set(content_, path, psize, nullptr);
    XBT_DEBUG("File '%s' was not found, file created.",path);
  }
  surf_file_t file = xbt_new0(s_surf_file_t,1);
  file->name = xbt_strdup(path);
  file->size = size;
  file->mount = xbt_strdup(mount);
  file->current_position = 0;

  StorageAction *action = new StorageN11Action(getModel(), 0, isOff(), this, OPEN);
  action->p_file = file;

  return action;
}

StorageAction *StorageN11::close(surf_file_t fd)
{
  XBT_DEBUG("\tClose file '%s' size '%llu'", fd->name, fd->size);
  // unref write actions from storage
  for (std::vector<StorageAction*>::iterator it = writeActions_.begin(); it != writeActions_.end();) {
    StorageAction *write_action = *it;
    if ((write_action->p_file) == fd) {
      write_action->unref();
      it = writeActions_.erase(it);
    } else {
      ++it;
    }
  }
  free(fd->name);
  free(fd->mount);
  xbt_free(fd);
  StorageAction *action = new StorageN11Action(getModel(), 0, isOff(), this, CLOSE);
  return action;
}

StorageAction *StorageN11::read(surf_file_t fd, sg_size_t size)
{
  if(fd->current_position + size > fd->size){
    if (fd->current_position > fd->size){
      size = 0;
    } else {
      size = fd->size - fd->current_position;
    }
    fd->current_position = fd->size;
  }
  else
    fd->current_position += size;

  StorageAction *action = new StorageN11Action(getModel(), size, isOff(), this, READ);
  return action;
}

StorageAction *StorageN11::write(surf_file_t fd, sg_size_t size)
{
  char *filename = fd->name;
  XBT_DEBUG("\tWrite file '%s' size '%llu/%llu'",filename,size,fd->size);

  StorageAction *action = new StorageN11Action(getModel(), size, isOff(), this, WRITE);
  action->p_file = fd;
  /* Substract the part of the file that might disappear from the used sized on the storage element */
  usedSize_ -= (fd->size - fd->current_position);
  // If the storage is full before even starting to write
  if(usedSize_==size_) {
    action->setState(Action::State::failed);
  }
  return action;
}

/**********
 * Action *
 **********/

StorageN11Action::StorageN11Action(Model *model, double cost, bool failed, Storage *storage, e_surf_action_storage_type_t type)
: StorageAction(model, cost, failed,
    lmm_variable_new(model->getMaxminSystem(), this, 1.0, -1.0 , 3),
    storage, type) {
  XBT_IN("(%s,%g", storage->getName(), cost);

  // Must be less than the max bandwidth for all actions
  lmm_expand(model->getMaxminSystem(), storage->getConstraint(), getVariable(), 1.0);
  switch(type) {
  case OPEN:
  case CLOSE:
  case STAT:
    break;
  case READ:
    lmm_expand(model->getMaxminSystem(), storage->constraintRead_, getVariable(), 1.0);
    break;
  case WRITE:
    lmm_expand(model->getMaxminSystem(), storage->constraintWrite_, getVariable(), 1.0);

    //TODO there is something annoying with what's below. Have to sort it out...
    //    Action *action = this;
    //    storage->p_writeActions->push_back(action);
    //    ref();
    break;
  }
  XBT_OUT();
}

int StorageN11Action::unref()
{
  refcount_--;
  if (!refcount_) {
    if (action_hook.is_linked())
      stateSet_->erase(stateSet_->iterator_to(*this));
    if (getVariable())
      lmm_variable_free(getModel()->getMaxminSystem(), getVariable());
    xbt_free(getCategory());
    delete this;
    return 1;
  }
  return 0;
}

void StorageN11Action::cancel()
{
  setState(Action::State::failed);
  return;
}

void StorageN11Action::suspend()
{
  XBT_IN("(%p)", this);
  if (suspended_ != 2) {
    lmm_update_variable_weight(getModel()->getMaxminSystem(), getVariable(), 0.0);
    suspended_ = 1;
  }
  XBT_OUT();
}

void StorageN11Action::resume()
{
  THROW_UNIMPLEMENTED;
}

bool StorageN11Action::isSuspended()
{
  return suspended_ == 1;
}

void StorageN11Action::setMaxDuration(double /*duration*/)
{
  THROW_UNIMPLEMENTED;
}

void StorageN11Action::setPriority(double /*priority*/)
{
  THROW_UNIMPLEMENTED;
}

}
}
