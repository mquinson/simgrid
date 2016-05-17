/* Copyright (c) 2006-2014. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "xbt/log.h"
#include "src/msg/msg_private.h"

#include "simgrid/s4u/actor.hpp"
#include "simgrid/s4u/comm.hpp"
#include "simgrid/s4u/host.hpp"
#include "simgrid/s4u/mailbox.hpp"

XBT_LOG_NEW_DEFAULT_CATEGORY(s4u_actor,"S4U actors");

/* C main function of a actor, running this->main */
int s4u_actor_runner(int argc, char **argv)
{
  simgrid::s4u::Actor *actor = static_cast<simgrid::s4u::Actor*>(SIMIX_process_self_get_data());
  actor->pimpl_ = SIMIX_process_self();
  int res = actor->main(argc,argv);
  return res;
}



using namespace simgrid;

s4u::Actor::Actor(smx_process_t smx_proc) : pimpl_(smx_proc) {
}

s4u::Actor::Actor() : pimpl_(NULL) { 
}

int s4u::Actor::main(int argc, char **argv) {
  fprintf(stderr,"Error: You should override the method main(int, char**) in Actor class %s\n",getName());
  return 0;
}
s4u::Actor &s4u::Actor::self()
{
  smx_process_t smx_proc = SIMIX_process_self();
  simgrid::s4u::Actor* res = (simgrid::s4u::Actor*) SIMIX_process_self_get_data();
  if (res == NULL) // The smx_process was not created by S4U (but by deployment?). Embed it in a S4U object
    res = new Actor(smx_proc);
  return *res;
}

void s4u::Actor::setAutoRestart(bool autorestart) {
  simcall_process_auto_restart_set(pimpl_,autorestart);
}

s4u::Host *s4u::Actor::getHost() {
  return s4u::Host::by_name(sg_host_get_name(simcall_process_get_host(pimpl_)));
}
const char* s4u::Actor::getName() {
  return simcall_process_get_name(pimpl_);
}
int s4u::Actor::getPid(){
  return simcall_process_get_PID(pimpl_);
}

void s4u::Actor::setKillTime(double time) {
  simcall_process_set_kill_time(pimpl_,time);
}
double s4u::Actor::getKillTime() {
  return simcall_process_get_kill_time(pimpl_);
}
void s4u::Actor::killAll() {
  simcall_process_killall(1);
}
void s4u::Actor::kill() {
  simcall_process_kill(pimpl_);
}

void s4u::Actor::sleep(double duration) {
  simcall_process_sleep(duration);
}

e_smx_state_t s4u::Actor::execute(double flops) {
  smx_synchro_t s = simcall_execution_start(NULL,flops,1.0/*priority*/,0./*bound*/, 0L/*affinity*/);
  return simcall_execution_wait(s);
}

void *s4u::Actor::recv(Mailbox &chan) {
  void *res=NULL;

  Comm c = Comm::recv_init(this, chan);
  c.setDstData(&res,sizeof(res));
  c.wait();

    return res;
}
void s4u::Actor::send(Mailbox &chan, void *payload, size_t simulatedSize) {
  Comm c = Comm::send_init(this,chan);
  c.setRemains(simulatedSize);
  c.setSrcData(payload);
  // c.start() is optional.
  c.wait();
}
