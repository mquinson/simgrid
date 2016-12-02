/* Copyright (c) 2013-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/base.h>

#include "storage_interface.hpp"

#ifndef STORAGE_N11_HPP_
#define STORAGE_N11_HPP_

namespace simgrid {
namespace surf {

/***********
 * Classes *
 ***********/

class XBT_PRIVATE StorageN11Model;
class XBT_PRIVATE StorageN11;
class XBT_PRIVATE StorageN11Action;

/*********
 * Model *
 *********/

class StorageN11Model : public StorageModel {
public:
  Storage* createStorage(const char* id, const char* type_id, const char* content_name, const char* content_type,
                         const char* attach) override;
  double nextOccuringEvent(double now) override;
  void updateActionsState(double now, double delta) override;
};

/************
 * Resource *
 ************/

class StorageN11 : public Storage {
public:
  StorageN11(StorageModel* model, const char* name, lmm_system_t maxminSystem, double bread, double bwrite,
             double bconnection, const char* type_id, char* content_name, const char* content_type, sg_size_t size,
             char* attach);

  StorageAction *open(const char* mount, const char* path);
  StorageAction *close(surf_file_t fd);
  StorageAction *ls(const char *path);
  StorageAction* read(surf_file_t fd, sg_size_t size);
  StorageAction* write(surf_file_t fd, sg_size_t size);
  void rename(const char *src, const char *dest);
};

/**********
 * Action *
 **********/

class StorageN11Action : public StorageAction {
public:
  StorageN11Action(Model *model, double cost, bool failed, Storage *storage, e_surf_action_storage_type_t type);
  void suspend();
  int unref();
  void cancel();
  void resume();
  bool isSuspended();
  void setMaxDuration(double duration);
  void setPriority(double priority);

};

}
}

#endif /* STORAGE_N11_HPP_ */
