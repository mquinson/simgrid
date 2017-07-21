/* Copyright (c) 2007-2017. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <xbt/ex.hpp>
#include <xbt/sysdep.h>
#include <xbt/log.h>

#include "simgrid/s4u/Host.hpp"
#include "simgrid/s4u/Storage.hpp"
#include "src/surf/FileImpl.hpp"
#include "src/surf/HostImpl.hpp"
#include "src/surf/StorageImpl.hpp"
#include "surf/surf.h"

#include "src/surf/surf_interface.hpp"
#include "smx_private.h"

#include "src/kernel/activity/SynchroIo.hpp"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(simix_io, simix, "Logging specific to SIMIX (io)");

//SIMIX FILE READ
void simcall_HANDLER_file_read(smx_simcall_t simcall, surf_file_t fd, sg_size_t size)
{
  smx_activity_t synchro = SIMIX_file_read(fd, size);
  synchro->simcalls.push_back(simcall);
  simcall->issuer->waiting_synchro = synchro;
}

smx_activity_t SIMIX_file_read(surf_file_t file, sg_size_t size)
{
  simgrid::kernel::activity::IoImpl* synchro = new simgrid::kernel::activity::IoImpl();
  synchro->surf_io                           = file->read(size);

  synchro->surf_io->setData(synchro);
  XBT_DEBUG("Create io synchro %p", synchro);

  return synchro;
}

//SIMIX FILE WRITE
void simcall_HANDLER_file_write(smx_simcall_t simcall, surf_file_t fd, sg_size_t size)
{
  smx_activity_t synchro = SIMIX_file_write(fd, size);
  synchro->simcalls.push_back(simcall);
  simcall->issuer->waiting_synchro = synchro;
}

smx_activity_t SIMIX_file_write(surf_file_t file, sg_size_t size)
{
  simgrid::kernel::activity::IoImpl* synchro = new simgrid::kernel::activity::IoImpl();
  synchro->surf_io                           = file->write(size);
  synchro->surf_io->setData(synchro);
  XBT_DEBUG("Create io synchro %p", synchro);

  return synchro;
}

void SIMIX_io_destroy(smx_activity_t synchro)
{
  simgrid::kernel::activity::IoImplPtr io = boost::static_pointer_cast<simgrid::kernel::activity::IoImpl>(synchro);
  XBT_DEBUG("Destroy synchro %p", synchro.get());
  if (io->surf_io)
    io->surf_io->unref();
}

void SIMIX_io_finish(smx_activity_t synchro)
{
  for (smx_simcall_t simcall : synchro->simcalls) {
    switch (synchro->state) {
      case SIMIX_DONE:
        /* do nothing, synchro done */
        break;
      case SIMIX_FAILED:
        SMX_EXCEPTION(simcall->issuer, io_error, 0, "IO failed");
        break;
      case SIMIX_CANCELED:
        SMX_EXCEPTION(simcall->issuer, cancel_error, 0, "Canceled");
        break;
      default:
        xbt_die("Internal error in SIMIX_io_finish: unexpected synchro state %d", static_cast<int>(synchro->state));
    }

    if (simcall->issuer->host->isOff()) {
      simcall->issuer->context->iwannadie = 1;
    }

    simcall->issuer->waiting_synchro = nullptr;
    SIMIX_simcall_answer(simcall);
  }

  /* We no longer need it */
  SIMIX_io_destroy(synchro);
}
