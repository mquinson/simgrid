/* Copyright (c) 2004-2017. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/base.h>
#include <xbt/signal.hpp>

#include "simgrid/s4u/Storage.hpp"
#include "src/surf/PropertyHolder.hpp"
#include "surf_interface.hpp"
#include <map>

#ifndef STORAGE_INTERFACE_HPP_
#define STORAGE_INTERFACE_HPP_

namespace simgrid {
namespace surf {

/***********
 * Classes *
 ***********/

class StorageAction;

/*************
 * Callbacks *
 *************/

/** @ingroup SURF_callbacks
 * @brief Callbacks handler which emit the callbacks after Storage creation *
 * @details Callback functions have the following signature: `void(Storage*)`
 */
XBT_PUBLIC_DATA(simgrid::xbt::signal<void(simgrid::surf::StorageImpl*)>) storageCreatedCallbacks;

/** @ingroup SURF_callbacks
 * @brief Callbacks handler which emit the callbacks after Storage destruction *
 * @details Callback functions have the following signature: `void(StoragePtr)`
 */
XBT_PUBLIC_DATA(simgrid::xbt::signal<void(simgrid::surf::StorageImpl*)>) storageDestructedCallbacks;

/** @ingroup SURF_callbacks
 * @brief Callbacks handler which emit the callbacks after Storage State changed *
 * @details Callback functions have the following signature: `void(StorageAction *action, int previouslyOn, int
 * currentlyOn)`
 */
XBT_PUBLIC_DATA(simgrid::xbt::signal<void(simgrid::surf::StorageImpl*, int, int)>) storageStateChangedCallbacks;

/** @ingroup SURF_callbacks
 * @brief Callbacks handler which emit the callbacks after StorageAction State changed *
 * @details Callback functions have the following signature: `void(StorageAction *action, simgrid::surf::Action::State
 * old, simgrid::surf::Action::State current)`
 */
XBT_PUBLIC_DATA(simgrid::xbt::signal<void(simgrid::surf::StorageAction*, simgrid::surf::Action::State,
                                          simgrid::surf::Action::State)>)
storageActionStateChangedCallbacks;

/*********
 * Model *
 *********/
/** @ingroup SURF_storage_interface
 * @brief SURF storage model interface class
 * @details A model is an object which handle the interactions between its Resources and its Actions
 */
class StorageModel : public Model {
public:
  StorageModel();
  ~StorageModel();

  virtual StorageImpl* createStorage(const char* id, const char* type_id, const char* content_name,
                                     const char* attach) = 0;

  std::vector<StorageImpl*> p_storageList;
};

/************
 * Resource *
 ************/
/** @ingroup SURF_storage_interface
 * @brief SURF storage interface class
 * @details A Storage represent a storage unit (e.g.: hard drive, usb key)
 */
class StorageImpl : public simgrid::surf::Resource, public simgrid::surf::PropertyHolder {
public:
  /** @brief Storage constructor */
  StorageImpl(Model* model, const char* name, lmm_system_t maxminSystem, double bread, double bwrite,
              const char* type_id, const char* content_name, sg_size_t size, const char* attach);

  ~StorageImpl() override;

  /** @brief Public interface */
  s4u::Storage piface_;
  static StorageImpl* byName(const char* name);

  /** @brief Check if the Storage is used (if an action currently uses its resources) */
  bool isUsed() override;

  void apply_event(tmgr_trace_event_t event, double value) override;

  void turnOn() override;
  void turnOff() override;

  /**
   * @brief Read a file
   *
   * @param size The size in bytes to read
   * @return The StorageAction corresponding to the reading
   */
  virtual StorageAction* read(sg_size_t size) = 0;

  /**
   * @brief Write a file
   *
   * @param size The size in bytes to write
   * @return The StorageAction corresponding to the writing
   */
  virtual StorageAction* write(sg_size_t size) = 0;

  /**
   * @brief Get the content of the current Storage
   *
   * @return A xbt_dict_t with path as keys and size in bytes as values
   */
  virtual std::map<std::string, sg_size_t>* getContent();

  /**
   * @brief Get the available size in bytes of the current Storage
   *
   * @return The available size in bytes of the current Storage
   */
  virtual sg_size_t getFreeSize();

  /**
   * @brief Get the used size in bytes of the current Storage
   *
   * @return The used size in bytes of the current Storage
   */
  virtual sg_size_t getUsedSize();
  virtual sg_size_t getSize() { return size_; }
  virtual std::string getHost() { return attach_; }

  std::map<std::string, sg_size_t>* parseContent(const char* filename);
  static std::unordered_map<std::string, StorageImpl*>* storagesMap() { return StorageImpl::storages; }

  lmm_constraint_t constraintWrite_; /* Constraint for maximum write bandwidth*/
  lmm_constraint_t constraintRead_;  /* Constraint for maximum write bandwidth*/

  std::string typeId_;
  sg_size_t usedSize_ = 0;

private:
  sg_size_t size_;
  static std::unordered_map<std::string, StorageImpl*>* storages;
  std::map<std::string, sg_size_t>* content_;
  // Name of the host to which this storage is attached. Only used at platform parsing time, then the interface stores
  // the Host directly.
  std::string attach_;
};

/**********
 * Action *
 **********/

/** @ingroup SURF_storage_interface
 * @brief The possible type of action for the storage component
 */
typedef enum {
  READ = 0, /**< Read a file */
  WRITE     /**< Write in a file */
} e_surf_action_storage_type_t;

/** @ingroup SURF_storage_interface
 * @brief SURF storage action interface class
 */
class StorageAction : public Action {
public:
  /**
   * @brief StorageAction constructor
   *
   * @param model The StorageModel associated to this StorageAction
   * @param cost The cost of this  NetworkAction in [TODO]
   * @param failed [description]
   * @param storage The Storage associated to this StorageAction
   * @param type [description]
   */
  StorageAction(Model* model, double cost, bool failed, StorageImpl* storage, e_surf_action_storage_type_t type)
      : Action(model, cost, failed), type_(type), storage_(storage){};

  /**
 * @brief StorageAction constructor
 *
 * @param model The StorageModel associated to this StorageAction
 * @param cost The cost of this  StorageAction in [TODO]
 * @param failed [description]
 * @param var The lmm variable associated to this StorageAction if it is part of a LMM component
 * @param storage The Storage associated to this StorageAction
 * @param type [description]
 */
  StorageAction(Model* model, double cost, bool failed, lmm_variable_t var, StorageImpl* storage,
                e_surf_action_storage_type_t type)
      : Action(model, cost, failed, var), type_(type), storage_(storage){};

  void setState(simgrid::surf::Action::State state) override;

  e_surf_action_storage_type_t type_;
  StorageImpl* storage_;
  FileImpl* file_ = nullptr;
};
}
}

typedef struct s_storage_type {
  char* model;
  char* content;
  char* type_id;
  xbt_dict_t properties;
  std::map<std::string, std::string>* model_properties;
  sg_size_t size;
} s_storage_type_t;
typedef s_storage_type_t* storage_type_t;

#endif /* STORAGE_INTERFACE_HPP_ */
