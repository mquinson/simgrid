/* Copyright (c) 2007-2025. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "smpi_request.hpp"

#include "private.hpp"
#include "simgrid/Exception.hpp"
#include "simgrid/s4u/ConditionVariable.hpp"
#include "simgrid/s4u/Exec.hpp"
#include "simgrid/s4u/Mailbox.hpp"
#include "simgrid/s4u/Mutex.hpp"
#include "smpi_comm.hpp"
#include "smpi_datatype.hpp"
#include "smpi_host.hpp"
#include "smpi_op.hpp"
#include "src/kernel/EngineImpl.hpp"
#include "src/kernel/activity/CommImpl.hpp"
#include "src/kernel/actor/ActorImpl.hpp"
#include "src/kernel/actor/WaitTestObserver.hpp"
#include "src/mc/mc.h"
#include "src/mc/mc_replay.hpp"
#include "src/smpi/include/smpi_actor.hpp"

#include <algorithm>
#include <array>
#include <mutex> // std::scoped_lock and std::unique_lock

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(smpi_request, smpi, "Logging specific to SMPI (request)");

static simgrid::config::Flag<double> smpi_iprobe_sleep(
  "smpi/iprobe", "Minimum time to inject inside a call to MPI_Iprobe", 1e-4);
static simgrid::config::Flag<double> smpi_test_sleep(
  "smpi/test", "Minimum time to inject inside a call to MPI_Test", 1e-4);

extern std::function<void(simgrid::kernel::activity::CommImpl*, void*, size_t)> smpi_comm_copy_data_callback;

namespace simgrid::smpi {

Request::Request(const void* buf, int count, MPI_Datatype datatype, aid_t src, aid_t dst, int tag, MPI_Comm comm,
                 unsigned flags, MPI_Op op)
    : buf_(const_cast<void*>(buf))
    , old_buf_(buf_)
    , type_(datatype)
    , size_(datatype->size() * count)
    , src_(src)
    , dst_(dst)
    , tag_(tag)
    , comm_(comm)
    , flags_(flags)
    , op_(op)
{
  datatype->ref();
  comm_->ref();
  if(op != MPI_REPLACE && op != MPI_OP_NULL)
    op_->ref();
  action_          = nullptr;
  detached_        = false;
  detached_sender_ = nullptr;
  real_src_        = 0;
  // get src_host if it's available (src is valid)
  if (auto src_process = simgrid::s4u::Actor::by_pid(src))
    src_host_ = src_process->get_host();
  truncated_       = false;
  unmatched_types_ = false;
  real_size_       = 0;
  real_tag_        = 0;
  if (flags & MPI_REQ_PERSISTENT)
    refcount_ = 1;
  else
    refcount_ = 0;
  init_buffer(count);
  this->add_f();
}

void Request::ref(){
  refcount_++;
}

void Request::unref(MPI_Request* request)
{
  xbt_assert(*request != MPI_REQUEST_NULL, "freeing an already free request");

  (*request)->refcount_--;
  if ((*request)->refcount_ < 0) {
    (*request)->print_request("wrong refcount");
    xbt_die("Whoops, wrong refcount");
  }
  if ((*request)->refcount_ == 0) {
    if ((*request)->flags_ & MPI_REQ_GENERALIZED) {
      ((*request)->generalized_funcs)->free_fn(((*request)->generalized_funcs)->extra_state);
    } else {
      Comm::unref((*request)->comm_);
      Datatype::unref((*request)->type_);
    }
    if ((*request)->op_ != MPI_REPLACE && (*request)->op_ != MPI_OP_NULL)
      Op::unref(&(*request)->op_);

    (*request)->print_request("Destroying");
    F2C::free_f((*request)->f2c_id());
    delete *request;
    *request = MPI_REQUEST_NULL;
  } else {
    (*request)->print_request("Decrementing");
  }
}

bool Request::match_types(MPI_Datatype stype, MPI_Datatype rtype){
  bool match = false;
  if ((stype == rtype) ||
     //byte and packed always match with anything
     (stype == MPI_PACKED || rtype == MPI_PACKED || stype == MPI_BYTE || rtype == MPI_BYTE) ||
     //complex datatypes - we don't properly match these yet, as it would mean checking each subtype recursively.
     (stype->flags() & DT_FLAG_DERIVED || rtype->flags() & DT_FLAG_DERIVED) ||
     //duplicated datatypes, check if underlying is ok
     (stype->duplicated_datatype()!=MPI_DATATYPE_NULL && match_types(stype->duplicated_datatype(), rtype)) ||
     (rtype->duplicated_datatype()!=MPI_DATATYPE_NULL && match_types(stype, rtype->duplicated_datatype())))
    match = true;
  if (not match)
    XBT_WARN("Mismatched datatypes : sending %s and receiving %s", stype->name().c_str(), rtype->name().c_str());
  return match;
}


bool Request::match_common(MPI_Request req, MPI_Request sender, MPI_Request receiver)
{
  xbt_assert(sender, "Cannot match against null sender");
  xbt_assert(receiver, "Cannot match against null receiver");
  XBT_DEBUG("Trying to match %s of sender src %ld against %ld, tag %d against %d, id %d against %d",
            (req == receiver ? "send" : "recv"), sender->src_, receiver->src_, sender->tag_, receiver->tag_,
            sender->comm_->id(), receiver->comm_->id());

  if ((receiver->comm_->id() == MPI_UNDEFINED || sender->comm_->id() == MPI_UNDEFINED ||
       receiver->comm_->id() == sender->comm_->id()) &&
      ((receiver->src_ == MPI_ANY_SOURCE && (receiver->comm_->group()->rank(sender->src_) != MPI_UNDEFINED)) ||
       receiver->src_ == sender->src_) &&
      ((receiver->tag_ == MPI_ANY_TAG && sender->tag_ >= 0) || receiver->tag_ == sender->tag_)) {
    // we match, we can transfer some values
    if (receiver->src_ == MPI_ANY_SOURCE) {
      receiver->real_src_ = sender->src_;
      receiver->src_host_ = sender->src_host_;
    }
    if (receiver->tag_ == MPI_ANY_TAG)
      receiver->real_tag_ = sender->tag_;
    if ((receiver->flags_ & MPI_REQ_PROBE) == 0 && receiver->real_size_ < sender->real_size_) {
      XBT_DEBUG("Truncating message - should not happen: receiver size : %zu < sender size : %zu", receiver->real_size_,
                sender->real_size_);
      receiver->truncated_ = true;
    }
    //0-sized datatypes/counts should not interfere and match
    if (sender->real_size_ != 0 && receiver->real_size_ != 0 && not match_types(sender->type_, receiver->type_))
      receiver->unmatched_types_ = true;
    if (sender->detached_)
      receiver->detached_sender_ = sender; // tie the sender to the receiver, as it is detached and has to be freed in
                                           // the receiver
    req->flags_ |= MPI_REQ_MATCHED; // mark as impossible to cancel anymore
    XBT_DEBUG("match succeeded");
    return true;
  }
  return false;
}

void Request::init_buffer(int count){
// FIXME Handle the case of a partial shared malloc.
  // This part handles the problem of non-contiguous memory (for the unserialization at the reception)
  if (not smpi_process()->replaying() &&
     ((((flags_ & MPI_REQ_RECV) != 0) && ((flags_ & MPI_REQ_ACCUMULATE) != 0)) || (type_->flags() & DT_FLAG_DERIVED))) {
    // This part handles the problem of non-contiguous memory
    old_buf_ = buf_;
    if (count==0){
      buf_ = nullptr;
    }else {
      buf_ = xbt_malloc(count*type_->size());
      if ((type_->flags() & DT_FLAG_DERIVED) && ((flags_ & MPI_REQ_SEND) != 0)) {
        type_->serialize(old_buf_, buf_, count);
      }
    }
  }
}

bool Request::match_recv(void* a, void* b, simgrid::kernel::activity::CommImpl*)
{
  auto* ref  = static_cast<MPI_Request>(a);
  auto* req  = static_cast<MPI_Request>(b);
  bool match = match_common(req, req, ref);
  if (not match || ref->comm_ == MPI_COMM_UNINITIALIZED || ref->comm_->is_smp_comm())
    return match;
  auto it = std::find(req->message_id_.begin(), req->message_id_.end(), ref->comm_->get_received_messages_count(ref->comm_->group()->rank(req->src_),
                                              ref->comm_->group()->rank(req->dst_), req->tag_));
  if (it != req->message_id_.end()) {
    if (((ref->flags_ & MPI_REQ_PROBE) == 0) && ((req->flags_ & MPI_REQ_PROBE) == 0)) {
      req->message_id_.erase(it);
      XBT_DEBUG("increasing count in comm %p, which was %u from pid %ld, to pid %ld with tag %d", ref->comm_,
                ref->comm_->get_received_messages_count(ref->comm_->group()->rank(req->src_),
                                                        ref->comm_->group()->rank(req->dst_), req->tag_),
                req->src_, req->dst_, req->tag_);
      ref->comm_->increment_received_messages_count(ref->comm_->group()->rank(req->src_),
                                                    ref->comm_->group()->rank(req->dst_), req->tag_);
      if (ref->real_size_ > req->real_size_) {
        ref->real_size_ = req->real_size_;
      }
    }
  } else {
    match = false;
    req->flags_ &= ~MPI_REQ_MATCHED;
    ref->detached_sender_ = nullptr;
    XBT_DEBUG("Refusing to match message, as its ID is not the one I expect. in comm %p, %u, "
              "from pid %ld to pid %ld, with tag %d",
              ref->comm_,
              ref->comm_->get_received_messages_count(ref->comm_->group()->rank(req->src_),
                                                      ref->comm_->group()->rank(req->dst_), req->tag_),
              req->src_, req->dst_, req->tag_);
  }
  return match;
}

bool Request::match_send(void* a, void* b, simgrid::kernel::activity::CommImpl*)
{
  auto* ref = static_cast<MPI_Request>(a);
  auto* req = static_cast<MPI_Request>(b);
  return match_common(req, ref, req);
}

void Request::print_request(const char* message) const
{
  XBT_VERB("%s  request %p  [buf = %p, size = %zu, src = %ld, dst = %ld, tag = %d, flags = %x]", message, this, buf_,
           size_, src_, dst_, tag_, flags_);
}

/* factories, to hide the internal flags from the caller */
MPI_Request Request::bsend_init(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  return new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                     dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                     MPI_REQ_PERSISTENT | MPI_REQ_SEND | MPI_REQ_PREPARED | MPI_REQ_BSEND);
}

MPI_Request Request::send_init(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  return new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                     dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                     MPI_REQ_PERSISTENT | MPI_REQ_SEND | MPI_REQ_PREPARED);
}

MPI_Request Request::ssend_init(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  return new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                     dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                     MPI_REQ_PERSISTENT | MPI_REQ_SSEND | MPI_REQ_SEND | MPI_REQ_PREPARED);
}

MPI_Request Request::isend_init(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  return new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                     dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                     MPI_REQ_PERSISTENT | MPI_REQ_ISEND | MPI_REQ_SEND | MPI_REQ_PREPARED);
}

MPI_Request Request::rma_send_init(const void *buf, int count, MPI_Datatype datatype, int src, int dst, int tag, MPI_Comm comm,
                               MPI_Op op)
{
  MPI_Request request;
  if(op==MPI_OP_NULL){
    request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, comm->group()->actor(src),
                          dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                          MPI_REQ_RMA | MPI_REQ_NON_PERSISTENT | MPI_REQ_ISEND | MPI_REQ_SEND | MPI_REQ_PREPARED);
  }else{
    request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, comm->group()->actor(src),
                          dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                          MPI_REQ_RMA | MPI_REQ_NON_PERSISTENT | MPI_REQ_ISEND | MPI_REQ_SEND | MPI_REQ_PREPARED |
                              MPI_REQ_ACCUMULATE,
                          op);
  }
  return request;
}

MPI_Request Request::recv_init(void *buf, int count, MPI_Datatype datatype, int src, int tag, MPI_Comm comm)
{
  aid_t source = MPI_PROC_NULL;
  if (src == MPI_ANY_SOURCE)
    source = MPI_ANY_SOURCE;
  else if (src != MPI_PROC_NULL)
    source = comm->group()->actor(src);
  return new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype,
                     source,
                     simgrid::s4u::this_actor::get_pid(), tag, comm,
                     MPI_REQ_PERSISTENT | MPI_REQ_RECV | MPI_REQ_PREPARED);
}

MPI_Request Request::rma_recv_init(void *buf, int count, MPI_Datatype datatype, int src, int dst, int tag, MPI_Comm comm,
                               MPI_Op op)
{
  aid_t source        = MPI_PROC_NULL;
  if (src == MPI_ANY_SOURCE)
    source = MPI_ANY_SOURCE;
  else if (src != MPI_PROC_NULL)
    source = comm->group()->actor(src);
  MPI_Request request;
  if(op==MPI_OP_NULL){
    request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, source,
                          dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                          MPI_REQ_RMA | MPI_REQ_NON_PERSISTENT | MPI_REQ_RECV | MPI_REQ_PREPARED);
  }else{
    request =
        new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, source,
                    dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                    MPI_REQ_RMA | MPI_REQ_NON_PERSISTENT | MPI_REQ_RECV | MPI_REQ_PREPARED | MPI_REQ_ACCUMULATE, op);
  }
  return request;
}

MPI_Request Request::irecv_init(void *buf, int count, MPI_Datatype datatype, int src, int tag, MPI_Comm comm)
{
  aid_t source = MPI_PROC_NULL;
  if (src == MPI_ANY_SOURCE)
    source = MPI_ANY_SOURCE;
  else if (src != MPI_PROC_NULL)
    source = comm->group()->actor(src);
  return new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype,
                     source, simgrid::s4u::this_actor::get_pid(), tag, comm,
                     MPI_REQ_PERSISTENT | MPI_REQ_RECV | MPI_REQ_PREPARED);
}

MPI_Request Request::ibsend(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  auto* request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                              dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                              MPI_REQ_NON_PERSISTENT | MPI_REQ_ISEND | MPI_REQ_SEND | MPI_REQ_BSEND);
  if(dst != MPI_PROC_NULL)
    request->start();
  return request;
}

MPI_Request Request::isend(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  auto* request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                              dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                              MPI_REQ_NON_PERSISTENT | MPI_REQ_ISEND | MPI_REQ_SEND);
  if(dst != MPI_PROC_NULL)
    request->start();
  return request;
}

MPI_Request Request::issend(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  auto* request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                              dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                              MPI_REQ_NON_PERSISTENT | MPI_REQ_ISEND | MPI_REQ_SSEND | MPI_REQ_SEND);
  if(dst != MPI_PROC_NULL)
    request->start();
  return request;
}

MPI_Request Request::irecv(void *buf, int count, MPI_Datatype datatype, int src, int tag, MPI_Comm comm)
{
  aid_t source        = MPI_PROC_NULL;
  if (src == MPI_ANY_SOURCE)
    source = MPI_ANY_SOURCE;
  else if (src != MPI_PROC_NULL)
    source = comm->group()->actor(src);
  auto* request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, source,
                              simgrid::s4u::this_actor::get_pid(), tag, comm, MPI_REQ_NON_PERSISTENT | MPI_REQ_RECV);
  if(src != MPI_PROC_NULL)
    request->start();
  return request;
}

int Request::recv(void *buf, int count, MPI_Datatype datatype, int src, int tag, MPI_Comm comm, MPI_Status * status)
{
  MPI_Request request = irecv(buf, count, datatype, src, tag, comm);
  int retval = wait(&request,status);
  return retval;
}

void Request::bsend(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  auto* request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                              dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                              MPI_REQ_NON_PERSISTENT | MPI_REQ_SEND | MPI_REQ_BSEND);

  if(dst != MPI_PROC_NULL)
   request->start();
  wait(&request, MPI_STATUS_IGNORE);
}

void Request::send(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  auto* request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                              dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                              MPI_REQ_NON_PERSISTENT | MPI_REQ_SEND);
  if(dst != MPI_PROC_NULL)
   request->start();
  wait(&request, MPI_STATUS_IGNORE);
}

void Request::ssend(const void *buf, int count, MPI_Datatype datatype, int dst, int tag, MPI_Comm comm)
{
  auto* request = new Request(buf == MPI_BOTTOM ? nullptr : buf, count, datatype, simgrid::s4u::this_actor::get_pid(),
                              dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL, tag, comm,
                              MPI_REQ_NON_PERSISTENT | MPI_REQ_SSEND | MPI_REQ_SEND);

  if(dst != MPI_PROC_NULL)
   request->start();
  wait(&request,MPI_STATUS_IGNORE);
}

void Request::sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,int dst, int sendtag,
                       void *recvbuf, int recvcount, MPI_Datatype recvtype, int src, int recvtag,
                       MPI_Comm comm, MPI_Status * status)
{
  aid_t source = MPI_PROC_NULL;
  if (src == MPI_ANY_SOURCE)
    source = MPI_ANY_SOURCE;
  else if (src != MPI_PROC_NULL)
    source = comm->group()->actor(src);
  aid_t destination = dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL;

  std::array<MPI_Request, 2> requests;
  std::array<MPI_Status, 2> stats;
  if (aid_t myid = simgrid::s4u::this_actor::get_pid(); (destination == myid) && (source == myid)) {
    Datatype::copy(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype);
    if (status != MPI_STATUS_IGNORE) {
      status->MPI_SOURCE = source;
      status->MPI_TAG    = recvtag;
      status->MPI_ERROR  = MPI_SUCCESS;
      status->count      = sendcount * sendtype->size();
    }
    return;
  }
  requests[0] = isend_init(sendbuf, sendcount, sendtype, dst, sendtag, comm);
  requests[1] = irecv_init(recvbuf, recvcount, recvtype, src, recvtag, comm);
  startall(2, requests.data());
  waitall(2, requests.data(), stats.data());
  unref(&requests[0]);
  unref(&requests[1]);
  if(status != MPI_STATUS_IGNORE) {
    // Copy receive status
    *status = stats[1];
  }
}

void Request::isendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype,int dst, int sendtag,
                       void *recvbuf, int recvcount, MPI_Datatype recvtype, int src, int recvtag,
                       MPI_Comm comm, MPI_Request* request)
{
  aid_t source = MPI_PROC_NULL;
  if (src == MPI_ANY_SOURCE)
    source = MPI_ANY_SOURCE;
  else if (src != MPI_PROC_NULL)
    source = comm->group()->actor(src);
  aid_t destination = dst != MPI_PROC_NULL ? comm->group()->actor(dst) : MPI_PROC_NULL;
  
  (*request) = new Request( nullptr, 0, MPI_BYTE,
                         src,dst, sendtag, comm, MPI_REQ_PERSISTENT|MPI_REQ_NBC);
  std::vector<MPI_Request> requests;
  if (aid_t myid = simgrid::s4u::this_actor::get_pid(); (destination == myid) && (source == myid)) {
    Datatype::copy(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype);
    return;
  }
  requests.push_back(isend_init(sendbuf, sendcount, sendtype, dst, sendtag, comm));
  requests.push_back(irecv_init(recvbuf, recvcount, recvtype, src, recvtag, comm));
  (*request)->start_nbc_requests(requests);
}

void Request::start()
{
  s4u::Mailbox* mailbox;

  xbt_assert(action_ == nullptr, "Cannot (re-)start unfinished communication");
  //reinitialize temporary buffer for persistent requests
  if(real_size_ > 0 && flags_ & MPI_REQ_FINISHED){
    buf_ = old_buf_;
    init_buffer(real_size_/type_->size());
  }
  flags_ &= ~MPI_REQ_PREPARED;
  flags_ &= ~MPI_REQ_FINISHED;
  this->ref();

  // we make a copy here, as the size is modified by simix, and we may reuse the request in another receive later
  real_size_=size_;
  if ((flags_ & MPI_REQ_RECV) != 0) {
    this->print_request("New recv");

    simgrid::smpi::ActorExt* process = smpi_process_remote(simgrid::s4u::Actor::by_pid(dst_));

    std::unique_lock<s4u::Mutex> mut_lock;
    if (smpi_cfg_async_small_thresh() != 0 || (flags_ & MPI_REQ_RMA) != 0)
      mut_lock = std::unique_lock(*process->mailboxes_mutex());

    bool is_probe = ((flags_ & MPI_REQ_PROBE) != 0);
    flags_ |= MPI_REQ_PROBE;

    if (smpi_cfg_async_small_thresh() == 0 && (flags_ & MPI_REQ_RMA) == 0) {
      mailbox = process->mailbox();
    } else if (((flags_ & MPI_REQ_RMA) != 0) || static_cast<int>(size_) < smpi_cfg_async_small_thresh()) {
      //We have to check both mailboxes (because SSEND messages are sent to the large mbox).
      //begin with the more appropriate one : the small one.
      mailbox = process->mailbox_small();
      XBT_DEBUG("Is there a corresponding send already posted in the small mailbox %s (in case of SSEND)?",
                mailbox->get_cname());
      simgrid::kernel::activity::ActivityImplPtr action =
          mailbox->iprobe(s4u::Mailbox::IprobeKind::RECV, &match_recv, static_cast<void*>(this));

      if (action == nullptr) {
        mailbox = process->mailbox();
        XBT_DEBUG("No, nothing in the small mailbox test the other one : %s", mailbox->get_cname());
        action = mailbox->iprobe(s4u::Mailbox::IprobeKind::RECV, &match_recv, static_cast<void*>(this));
        if (action == nullptr) {
          XBT_DEBUG("Still nothing, switch back to the small mailbox : %s", mailbox->get_cname());
          mailbox = process->mailbox_small();
        }
      } else {
        XBT_DEBUG("yes there was something for us in the small mailbox");
      }
    } else {
      mailbox = process->mailbox_small();
      XBT_DEBUG("Is there a corresponding send already posted the small mailbox?");
      simgrid::kernel::activity::ActivityImplPtr action =
          mailbox->iprobe(s4u::Mailbox::IprobeKind::RECV, &match_recv, static_cast<void*>(this));

      if (action == nullptr) {
        XBT_DEBUG("No, nothing in the permanent receive mailbox");
        mailbox = process->mailbox();
      } else {
        XBT_DEBUG("yes there was something for us in the small mailbox");
      }
    }
    if (not is_probe)
      flags_ &= ~MPI_REQ_PROBE;
    kernel::actor::CommIrecvSimcall observer{process->get_actor()->get_impl(),
                                             mailbox->get_impl(),
                                             static_cast<unsigned char*>(buf_),
                                             &real_size_,
                                             &match_recv,
                                             process->replaying() ? &smpi_comm_null_copy_buffer_callback
                                                                  : smpi_comm_copy_data_callback,
                                             this,
                                             -1.0,
                                             process->call_location()->get_call_location()};
    observer.set_tag(tag_);

    action_ = kernel::actor::simcall_answered([&observer] { return kernel::activity::CommImpl::irecv(&observer); },
                                              &observer);

    XBT_DEBUG("recv simcall posted");
  } else { /* the RECV flag was not set, so this is a send */
    simgrid::smpi::ActorExt* process = smpi_process_remote(simgrid::s4u::Actor::by_pid(dst_));
    xbt_assert(process, "Actor pid=%ld is gone??", dst_);
    if (TRACE_smpi_view_internals())
      TRACE_smpi_send(src_, src_, dst_, tag_, size_);
    this->print_request("New send");

    message_id_.push_back(comm_->get_sent_messages_count(comm_->group()->rank(src_), comm_->group()->rank(dst_), tag_));
    comm_->increment_sent_messages_count(comm_->group()->rank(src_), comm_->group()->rank(dst_), tag_);

    void* buf = buf_;
    if ((flags_ & MPI_REQ_SSEND) == 0 && ((flags_ & MPI_REQ_RMA) != 0 || (flags_ & MPI_REQ_BSEND) != 0 ||
                                          static_cast<int>(size_) < smpi_cfg_detached_send_thresh())) {
      detached_    = true;
      XBT_DEBUG("Send request %p is detached", this);
      this->ref();
      if (not(type_->flags() & DT_FLAG_DERIVED)) {
        void* oldbuf = buf_;
        if (not process->replaying() && oldbuf != nullptr && size_ != 0) {
          if (smpi_switch_data_segment(simgrid::s4u::Actor::by_pid(src_), buf_))
            XBT_DEBUG("Privatization : We are sending from a zone inside global memory. Switch data segment ");

          //we need this temporary buffer even for bsend, as it will be released in the copy callback and we don't have a way to differentiate it
          //so actually ... don't use manually attached buffer space.
          buf = xbt_malloc(size_);
          memcpy(buf,oldbuf,size_);
          XBT_DEBUG("buf %p copied into %p",oldbuf,buf);
        }
      }
    }

    //if we are giving back the control to the user without waiting for completion, we have to inject timings
    double sleeptime = 0.0;
    if (detached_ || ((flags_ & (MPI_REQ_ISEND | MPI_REQ_SSEND)) != 0)) { // issend should be treated as isend
      // isend and send timings may be different
      sleeptime =
          ((flags_ & MPI_REQ_ISEND) != 0)
              ? simgrid::s4u::Actor::self()->get_host()->extension<simgrid::smpi::Host>()->oisend(
                    size_, simgrid::s4u::Actor::by_pid(src_)->get_host(), simgrid::s4u::Actor::by_pid(dst_)->get_host())
              : simgrid::s4u::Actor::self()->get_host()->extension<simgrid::smpi::Host>()->osend(
                    size_, simgrid::s4u::Actor::by_pid(src_)->get_host(),
                    simgrid::s4u::Actor::by_pid(dst_)->get_host());
    }

    if(sleeptime > 0.0){
      simgrid::s4u::this_actor::sleep_for(sleeptime);
      XBT_DEBUG("sending size of %zu : sleep %f ", size_, sleeptime);
    }

    std::unique_lock<s4u::Mutex> mut_lock;
    if (smpi_cfg_async_small_thresh() != 0 || (flags_ & MPI_REQ_RMA) != 0)
      mut_lock = std::unique_lock(*process->mailboxes_mutex());

    if (not(smpi_cfg_async_small_thresh() != 0 || (flags_ & MPI_REQ_RMA) != 0)) {
      mailbox = process->mailbox();
    } else if (((flags_ & MPI_REQ_RMA) != 0) || static_cast<int>(size_) < smpi_cfg_async_small_thresh()) { // eager mode
      bool is_probe = ((flags_ & MPI_REQ_PROBE) != 0);
      flags_ |= MPI_REQ_PROBE;

      mailbox = process->mailbox();
      XBT_DEBUG("Is there a corresponding recv already posted in the large mailbox %s?", mailbox->get_cname());
      if (not mailbox->iprobe(s4u::Mailbox::IprobeKind::SEND, &match_send, static_cast<void*>(this))) {
        if ((flags_ & MPI_REQ_SSEND) == 0) {
          mailbox = process->mailbox_small();
          XBT_DEBUG("No, nothing in the large mailbox, message is to be sent on the small one %s",
                    mailbox->get_cname());
        } else {
          mailbox = process->mailbox_small();
          XBT_DEBUG("SSEND : Is there a corresponding recv already posted in the small mailbox %s?",
                    mailbox->get_cname());
          if (not mailbox->iprobe(s4u::Mailbox::IprobeKind::SEND, &match_send, static_cast<void*>(this))) {
            XBT_DEBUG("No, we are first, send to large mailbox");
            mailbox = process->mailbox();
          }
        }
      } else {
        XBT_DEBUG("Yes there was something for us in the large mailbox");
      }
      if (not is_probe)
        flags_ &= ~MPI_REQ_PROBE;
    } else {
      mailbox = process->mailbox();
      XBT_DEBUG("Send request %p is in the large mailbox %s (buf: %p)", this, mailbox->get_cname(), buf_);
    }

    size_t payload_size_ = size_ + 16;//MPI enveloppe size (tag+dest+communicator)
    kernel::actor::CommIsendSimcall observer{
        simgrid::kernel::EngineImpl::get_instance()->get_actor_by_pid(src_), mailbox->get_impl(),
        static_cast<double>(payload_size_), -1, static_cast<unsigned char*>(buf), real_size_, &match_send,
        &xbt_free_f, // how to free the userdata if a detached send fails
        process->replaying() ? &smpi_comm_null_copy_buffer_callback : smpi_comm_copy_data_callback, this,
        // detach if msg size < eager/rdv switch limit
        detached_, process->call_location()->get_call_location()};
    observer.set_tag(tag_);
    action_ = kernel::actor::simcall_answered([&observer] { return kernel::activity::CommImpl::isend(&observer); },
                                              &observer);
    XBT_DEBUG("send simcall posted");

    /* FIXME: detached sends are not traceable (action_ == nullptr) */
    if (action_ != nullptr) {
      boost::static_pointer_cast<kernel::activity::CommImpl>(action_)->set_tracing_category(
          smpi_process()->get_tracing_category());
    }
  }
}

void Request::startall(int count, MPI_Request * requests)
{
  if(requests== nullptr)
    return;

  for(int i = 0; i < count; i++) {
    if(requests[i]->src_ != MPI_PROC_NULL && requests[i]->dst_ != MPI_PROC_NULL)
      requests[i]->start();
  }
}

void Request::cancel()
{
  this->flags_ |= MPI_REQ_CANCELLED;
  if (this->action_ != nullptr)
    (boost::static_pointer_cast<simgrid::kernel::activity::CommImpl>(this->action_))->cancel();
}

int Request::test(MPI_Request * request, MPI_Status * status, int* flag) {
  // assume that *request is not MPI_REQUEST_NULL (filtered in PMPI_Test or testall before)
  // to avoid deadlocks if used as a break condition, such as
  //     while (MPI_Test(request, flag, status) && flag) dostuff...
  // because the time will not normally advance when only calls to MPI_Test are made -> deadlock
  // multiplier to the sleeptime, to increase speed of execution, each failed test will increase it
  xbt_assert(*request != MPI_REQUEST_NULL);

  static int nsleeps = 1;
  int ret = MPI_SUCCESS;

  if(smpi_test_sleep > 0)
    simgrid::s4u::this_actor::sleep_for(nsleeps * smpi_test_sleep);

  Status::empty(status);
  *flag = 1;

  if ((*request)->flags_ & MPI_REQ_NBC){
    *flag = finish_nbc_requests(request, 1);
  }

  if (((*request)->flags_ & (MPI_REQ_PREPARED | MPI_REQ_FINISHED)) == 0) {
    if ((*request)->action_ != nullptr && ((*request)->flags_ & MPI_REQ_CANCELLED) == 0){
      try{
        kernel::actor::ActorImpl* issuer = kernel::actor::ActorImpl::self();
        simgrid::smpi::ActorExt* process = smpi_process_remote(simgrid::s4u::Actor::by_pid(issuer->get_pid()));
        kernel::actor::ActivityTestSimcall observer{issuer, (*request)->action_.get(),
                                                    process->call_location()->get_call_location()};
        *flag = kernel::actor::simcall_answered(
            [&observer] { return observer.get_activity()->test(observer.get_issuer()); }, &observer);
      } catch (const Exception&) {
        *flag = 0;
        return ret;
      }
    }
    if (((*request)->flags_ & MPI_REQ_GENERALIZED) && not((*request)->flags_ & MPI_REQ_COMPLETE))
      *flag=0;
    if (*flag) {
      finish_wait(request, status); // may invalidate *request
      if (*request != MPI_REQUEST_NULL && ((*request)->flags_ & MPI_REQ_GENERALIZED)){
        MPI_Status tmp_status;
        MPI_Status* mystatus;
        if (status == MPI_STATUS_IGNORE) {
          mystatus = &tmp_status;
          Status::empty(mystatus);
        } else {
          mystatus = status;
        }
        ret = ((*request)->generalized_funcs)->query_fn(((*request)->generalized_funcs)->extra_state, mystatus);
      }
      nsleeps=1;//reset the number of sleeps we will do next time
      if (*request != MPI_REQUEST_NULL && ((*request)->flags_ & MPI_REQ_PERSISTENT) == 0)
        *request = MPI_REQUEST_NULL;
    } else if (smpi_cfg_grow_injected_times()) {
      nsleeps++;
    }
  }
  return ret;
}

int Request::testsome(int incount, MPI_Request requests[], int *count, int *indices, MPI_Status status[])
{
  int error=0;
  int count_dead = 0;
  int flag = 0;
  MPI_Status stat;
  MPI_Status *pstat = status == MPI_STATUSES_IGNORE ? MPI_STATUS_IGNORE : &stat;

  *count = 0;
  for (int i = 0; i < incount; i++) {
    if (requests[i] != MPI_REQUEST_NULL && not (requests[i]->flags_ & MPI_REQ_FINISHED)) {
      if (test(&requests[i], pstat, &flag) != MPI_SUCCESS)
        error = 1;
      if(flag) {
        indices[*count] = i;
        if (status != MPI_STATUSES_IGNORE)
          status[*count] = *pstat;
        (*count)++;
        if ((requests[i] != MPI_REQUEST_NULL) && (requests[i]->flags_ & MPI_REQ_NON_PERSISTENT))
          requests[i] = MPI_REQUEST_NULL;
      }
    } else {
      count_dead++;
    }
  }
  if(count_dead==incount)*count=MPI_UNDEFINED;
  if(error!=0)
    return MPI_ERR_IN_STATUS;
  else
    return MPI_SUCCESS;
}

int Request::testany(int count, MPI_Request requests[], int *index, int* flag, MPI_Status * status)
{
  std::vector<simgrid::kernel::activity::ActivityImpl*> comms;
  comms.reserve(count);

  *flag = 0;
  int ret = MPI_SUCCESS;
  *index = MPI_UNDEFINED;

  std::vector<int> map; /** Maps all matching comms back to their location in requests **/
  for (int i = 0; i < count; i++) {
    if ((requests[i] != MPI_REQUEST_NULL) && requests[i]->action_ && not(requests[i]->flags_ & MPI_REQ_PREPARED)) {
      comms.push_back(requests[i]->action_.get());
      map.push_back(i);
    }
  }
  if (not map.empty()) {
    //multiplier to the sleeptime, to increase speed of execution, each failed testany will increase it
    static int nsleeps = 1;
    if(smpi_test_sleep > 0)
      simgrid::s4u::this_actor::sleep_for(nsleeps * smpi_test_sleep);
    ssize_t i;
    try{
      kernel::actor::ActorImpl* issuer = kernel::actor::ActorImpl::self();
      simgrid::smpi::ActorExt* process = smpi_process_remote(simgrid::s4u::Actor::by_pid(issuer->get_pid()));
      kernel::actor::ActivityTestanySimcall observer{issuer, comms, process->call_location()->get_call_location()};
      i = kernel::actor::simcall_answered(
          [&observer] {
            return kernel::activity::ActivityImpl::test_any(observer.get_issuer(), observer.get_activities());
          },
          &observer);
    } catch (const Exception&) {
      XBT_DEBUG("Exception in testany");
      return 0;
    }

    if (i != -1) { // -1 is not MPI_UNDEFINED but a SIMIX return code. (nothing matches)
      *index = map[i];
      if (requests[*index] != MPI_REQUEST_NULL && (requests[*index]->flags_ & MPI_REQ_GENERALIZED) &&
          not(requests[*index]->flags_ & MPI_REQ_COMPLETE)) {
        *flag=0;
      } else {
        finish_wait(&requests[*index],status);
      if (requests[*index] != MPI_REQUEST_NULL && (requests[*index]->flags_ & MPI_REQ_GENERALIZED)){
        MPI_Status tmp_status;
        MPI_Status* mystatus;
        if (status == MPI_STATUS_IGNORE) {
          mystatus = &tmp_status;
          Status::empty(mystatus);
        } else {
          mystatus = status;
        }
        ret=(requests[*index]->generalized_funcs)->query_fn((requests[*index]->generalized_funcs)->extra_state, mystatus);
      }

      if (requests[*index] != MPI_REQUEST_NULL && requests[*index]->flags_ & MPI_REQ_NBC){
        *flag = finish_nbc_requests(&requests[*index] , 1);
      }

      if (requests[*index] != MPI_REQUEST_NULL && (requests[*index]->flags_ & MPI_REQ_NON_PERSISTENT))
          requests[*index] = MPI_REQUEST_NULL;
        XBT_DEBUG("Testany - returning with index %d", *index);
        *flag=1;
      }
      nsleeps = 1;
    } else {
      nsleeps++;
    }
  } else {
      XBT_DEBUG("Testany on inactive handles, returning flag=1 but empty status");
      //all requests are null or inactive, return true
      *flag = 1;
      *index = MPI_UNDEFINED;
      Status::empty(status);
  }

  return ret;
}

int Request::testall(int count, MPI_Request requests[], int* outflag, MPI_Status status[])
{
  MPI_Status stat;
  MPI_Status *pstat = status == MPI_STATUSES_IGNORE ? MPI_STATUS_IGNORE : &stat;
  int flag;
  int error = 0;
  *outflag = 1;
  for(int i=0; i<count; i++){
    if (requests[i] != MPI_REQUEST_NULL && not(requests[i]->flags_ & MPI_REQ_PREPARED)) {
      int ret = test(&requests[i], pstat, &flag);
      if (flag){
        flag=0;
      }else{
        *outflag=0;
      }
      if (ret != MPI_SUCCESS)
        error = 1;
    }else{
      Status::empty(pstat);
    }
    if(status != MPI_STATUSES_IGNORE) {
      status[i] = *pstat;
    }
  }
  if (error == 1)
    return MPI_ERR_IN_STATUS;
  else
    return MPI_SUCCESS;
}

void Request::probe(int source, int tag, MPI_Comm comm, MPI_Status* status){
  int flag=0;
  //FIXME find another way to avoid busy waiting ?
  // the issue here is that we have to wait on a nonexistent comm
  xbt_assert(not(MC_is_active() || MC_record_replay_is_active()), "MPI_Probe is not supported yet in MC mode.");
  while(flag==0){
    iprobe(source, tag, comm, &flag, status);
    XBT_DEBUG("Busy Waiting on probing : %d", flag);
  }
}

void Request::iprobe(int source, int tag, MPI_Comm comm, int* flag, MPI_Status* status){
  // to avoid deadlock, we have to sleep some time here, or the timer won't advance and we will only do iprobe simcalls
  // especially when used as a break condition, such as while (MPI_Iprobe(...)) dostuff...
  // nsleeps is a multiplier to the sleeptime, to increase speed of execution, each failed iprobe will increase it
  // This can speed up the execution of certain applications by an order of magnitude, such as HPL
  static int nsleeps = 1;
  double speed        = s4u::this_actor::get_host()->get_speed();
  double maxrate      = smpi_cfg_iprobe_cpu_usage();
  auto* request =
      new Request(nullptr, 0, MPI_CHAR, source == MPI_ANY_SOURCE ? MPI_ANY_SOURCE : comm->group()->actor(source),
                  simgrid::s4u::this_actor::get_pid(), tag, comm, MPI_REQ_PERSISTENT | MPI_REQ_RECV | MPI_REQ_PROBE);
  if (smpi_iprobe_sleep > 0 && not(MC_is_active() || MC_record_replay_is_active())) {
    /** Compute the number of flops we will sleep **/
    s4u::this_actor::exec_init(/*nsleeps: See comment above */ nsleeps *
                               /*(seconds * flop/s -> total flops)*/ smpi_iprobe_sleep * speed * maxrate)
        ->set_name("iprobe")
        /* Not the entire CPU can be used when iprobing: This is important for
         * the energy consumption caused by polling with iprobes.
         * Note also that the number of flops that was
         * computed above contains a maxrate factor and is hence reduced (maxrate < 1)
         */
        ->set_bound(maxrate * speed)
        ->start()
        ->wait();
  }
  // behave like a receive, but don't do it
  s4u::Mailbox* mailbox;

  request->print_request("New iprobe");
  // We have to test both mailboxes as we don't know if we will receive one or another
  if (smpi_cfg_async_small_thresh() > 0) {
    mailbox = smpi_process()->mailbox_small();
    XBT_DEBUG("Trying to probe the perm recv mailbox");
    request->action_ = mailbox->iprobe(s4u::Mailbox::IprobeKind::RECV, &match_recv, static_cast<void*>(request));
  }

  if (request->action_ == nullptr) {
    mailbox = smpi_process()->mailbox();
    XBT_DEBUG("trying to probe the other mailbox");
    request->action_ = mailbox->iprobe(s4u::Mailbox::IprobeKind::RECV, &match_recv, static_cast<void*>(request));
  }

  if (request->action_ != nullptr) {
    kernel::activity::CommImplPtr sync_comm = boost::static_pointer_cast<kernel::activity::CommImpl>(request->action_);
    const Request* req                      = static_cast<MPI_Request>(sync_comm->src_match_data_);
    *flag = 1;
    if (status != MPI_STATUS_IGNORE && (req->flags_ & MPI_REQ_PREPARED) == 0) {
      status->MPI_SOURCE = comm->group()->rank(req->src_);
      status->MPI_TAG    = req->tag_;
      status->MPI_ERROR  = MPI_SUCCESS;
      status->count      = req->real_size_;
    }
    nsleeps = 1;//reset the number of sleeps we will do next time
  } else {
    *flag = 0;
    if (smpi_cfg_grow_injected_times())
      nsleeps++;
  }
  unref(&request);
  xbt_assert(request == MPI_REQUEST_NULL);
}

int Request::finish_nbc_requests(MPI_Request* request, int test){
  int flag = 1;
  int ret = 0;
  if(test == 0)
    ret = waitall((*request)->nbc_requests_.size(), (*request)->nbc_requests_.data(), MPI_STATUSES_IGNORE);
  else{
    ret = testall((*request)->nbc_requests_.size(), (*request)->nbc_requests_.data(), &flag, MPI_STATUSES_IGNORE);
  }
  if(ret!=MPI_SUCCESS)
    xbt_die("Failure when waiting on non blocking collective sub-requests");
  if(flag == 1){
    XBT_DEBUG("Finishing non blocking collective request with %zu sub-requests", (*request)->nbc_requests_.size());
    for(auto& req: (*request)->nbc_requests_){
      if((*request)->buf_!=nullptr && req!=MPI_REQUEST_NULL){//reduce case
        void * buf=req->buf_;
        if((*request)->type_->flags() & DT_FLAG_DERIVED)
          buf=req->old_buf_;
        if(req->flags_ & MPI_REQ_RECV ){
          if((*request)->op_!=MPI_OP_NULL){
            int count=(*request)->size_/ (*request)->type_->size();
            (*request)->op_->apply(buf, (*request)->buf_, &count, (*request)->type_);
          }
          smpi_free_tmp_buffer(static_cast<unsigned char*>(buf));
        }
      }
      if(req!=MPI_REQUEST_NULL)
        Request::unref(&req);
    }
    (*request)->nbc_requests_.clear();
  }
  return flag;
}

void Request::finish_wait(MPI_Request* request, MPI_Status * status)
{
  MPI_Request req = *request;
  Status::empty(status);
  if((req->flags_ & MPI_REQ_CANCELLED) != 0 && (req->flags_ & MPI_REQ_MATCHED) == 0) {
    if (status!=MPI_STATUS_IGNORE)
      status->cancelled=1;
    if(req->detached_sender_ != nullptr)
      unref(&(req->detached_sender_));
    unref(request);
    return;
  }

  if ((req->flags_ & (MPI_REQ_PREPARED | MPI_REQ_GENERALIZED | MPI_REQ_FINISHED)) == 0) {
    if (status != MPI_STATUS_IGNORE) {
      if (req->src_== MPI_PROC_NULL || req->dst_== MPI_PROC_NULL){
        Status::empty(status);
        status->MPI_SOURCE = MPI_PROC_NULL;
      } else {
        aid_t src          = req->src_ == MPI_ANY_SOURCE ? req->real_src_ : req->src_;
        status->MPI_SOURCE = req->comm_->group()->rank(src);
        status->MPI_TAG = req->tag_ == MPI_ANY_TAG ? req->real_tag_ : req->tag_;
        status->MPI_ERROR  = req->truncated_ ? MPI_ERR_TRUNCATE : MPI_SUCCESS;
      }
      // this handles the case were size in receive differs from size in send
      status->count = req->real_size_;
    }
    //detached send will be finished at the other end
    if (not(req->detached_ && ((req->flags_ & MPI_REQ_SEND) != 0))) {
      req->print_request("Finishing");
      MPI_Datatype datatype = req->type_;

      // FIXME Handle the case of a partial shared malloc.
      if (not smpi_process()->replaying() &&
        (((req->flags_ & MPI_REQ_ACCUMULATE) != 0) || (datatype->flags() & DT_FLAG_DERIVED))) {
        if (smpi_switch_data_segment(simgrid::s4u::Actor::self(), req->old_buf_))
          XBT_VERB("Privatization : We are unserializing to a zone in global memory  Switch data segment ");

        if(datatype->flags() & DT_FLAG_DERIVED){
          // This part handles the problem of non-contiguous memory the unserialization at the reception
          if ((req->flags_ & MPI_REQ_RECV) && datatype->size() != 0)
            datatype->unserialize(req->buf_, req->old_buf_, req->real_size_/datatype->size() , req->op_);
          xbt_free(req->buf_);
          req->buf_=nullptr;
        } else if (req->flags_ & MPI_REQ_RECV) { // apply op on contiguous buffer for accumulate
          if (datatype->size() != 0) {
            int n = req->real_size_ / datatype->size();
            req->op_->apply(req->buf_, req->old_buf_, &n, datatype);
          }
          xbt_free(req->buf_);
          req->buf_=nullptr;
        }
      }
    }
  }

  if (TRACE_smpi_view_internals() && ((req->flags_ & MPI_REQ_RECV) != 0)) {
    aid_t rank       = simgrid::s4u::this_actor::get_pid();
    aid_t src_traced = (req->src_ == MPI_ANY_SOURCE ? req->real_src_ : req->src_);
    TRACE_smpi_recv(src_traced, rank,req->tag_);
  }
  if(req->detached_sender_ != nullptr){
    //integrate pseudo-timing for buffering of small messages, do not bother to execute the simcall if 0
    simgrid::s4u::Host* dst_host = simgrid::s4u::Actor::by_pid(req->dst_)->get_host();
    if (double sleeptime = simgrid::s4u::Actor::self()->get_host()->extension<simgrid::smpi::Host>()->orecv(
            req->real_size(), req->src_host_, dst_host);
        sleeptime > 0.0) {
      simgrid::s4u::this_actor::sleep_for(sleeptime);
      XBT_DEBUG("receiving size of %zu : sleep %f ", req->real_size_, sleeptime);
    }
    unref(&(req->detached_sender_));
  }
  if (req->flags_ & MPI_REQ_PERSISTENT)
    req->action_ = nullptr;
  req->flags_ |= MPI_REQ_FINISHED;

  if (req->truncated_ || req->unmatched_types_) {
    char error_string[MPI_MAX_ERROR_STRING];
    int error_size;
    int errkind;
    if(req->truncated_ )
      errkind = MPI_ERR_TRUNCATE;
    else
      errkind = MPI_ERR_TYPE;
    PMPI_Error_string(errkind, error_string, &error_size);
    MPI_Errhandler err = (req->comm_) ? (req->comm_)->errhandler() : MPI_ERRHANDLER_NULL;
    if (err == MPI_ERRHANDLER_NULL || err == MPI_ERRORS_RETURN)
      XBT_WARN("recv - returned %.*s instead of MPI_SUCCESS", error_size, error_string);
    else if (err == MPI_ERRORS_ARE_FATAL)
      xbt_die("recv - returned %.*s instead of MPI_SUCCESS", error_size, error_string);
    else
      err->call((req->comm_), errkind);
    if (err != MPI_ERRHANDLER_NULL)
      simgrid::smpi::Errhandler::unref(err);
    MC_assert(not MC_is_active()); /* Only fail in MC mode */
  }
  if(req->src_ != MPI_PROC_NULL && req->dst_ != MPI_PROC_NULL)
    unref(request);
}

int Request::wait(MPI_Request * request, MPI_Status * status)
{
  // assume that *request is not MPI_REQUEST_NULL (filtered in PMPI_Wait before)
  xbt_assert(*request != MPI_REQUEST_NULL);

  int ret=MPI_SUCCESS;

  if((*request)->src_ == MPI_PROC_NULL || (*request)->dst_ == MPI_PROC_NULL){
    if (status != MPI_STATUS_IGNORE) {
      Status::empty(status);
      status->MPI_SOURCE = MPI_PROC_NULL;
    }
    (*request)=MPI_REQUEST_NULL;
    return ret;
  }

  (*request)->print_request("Waiting");
  if ((*request)->flags_ & (MPI_REQ_PREPARED | MPI_REQ_FINISHED)) {
    Status::empty(status);
    return ret;
  }

  if ((*request)->action_ != nullptr){
      try{
        // this is not a detached send
        kernel::actor::ActorImpl* issuer = kernel::actor::ActorImpl::self();
        simgrid::smpi::ActorExt* process = smpi_process_remote(simgrid::s4u::Actor::by_pid(issuer->get_pid()));
        kernel::actor::ActivityWaitSimcall observer{issuer, (*request)->action_.get(), -1,
                                                    process->call_location()->get_call_location()};
        kernel::actor::simcall_blocking([issuer, &observer] { observer.get_activity()->wait_for(issuer, -1); },
                                        &observer);
      } catch (const CancelException&) {
        XBT_VERB("Request cancelled");
      }
  }

  if ((*request)->flags_ & MPI_REQ_GENERALIZED) {
    if (not((*request)->flags_ & MPI_REQ_COMPLETE)) {
      const std::scoped_lock lock(*(*request)->generalized_funcs->mutex);
      (*request)->generalized_funcs->cond->wait((*request)->generalized_funcs->mutex);
    }
    MPI_Status tmp_status;
    MPI_Status* mystatus;
    if (status == MPI_STATUS_IGNORE) {
      mystatus = &tmp_status;
      Status::empty(mystatus);
    } else {
      mystatus = status;
    }
    ret = ((*request)->generalized_funcs)->query_fn(((*request)->generalized_funcs)->extra_state, mystatus);
  }

  if ((*request)->truncated_)
    ret = MPI_ERR_TRUNCATE;

  if ((*request)->flags_ & MPI_REQ_NBC)
    finish_nbc_requests(request, 0);

  finish_wait(request, status); // may invalidate *request
  if (*request != MPI_REQUEST_NULL && (((*request)->flags_ & MPI_REQ_NON_PERSISTENT) != 0))
    *request = MPI_REQUEST_NULL;
  return ret;
}

int Request::waitany(int count, MPI_Request requests[], MPI_Status * status)
{
  int index = MPI_UNDEFINED;

  if(count > 0) {
    // Wait for a request to complete
    std::vector<simgrid::kernel::activity::ActivityImpl*> comms;
    std::vector<int> map;
    XBT_DEBUG("Wait for one of %d", count);
    for(int i = 0; i < count; i++) {
      if (requests[i] != MPI_REQUEST_NULL && not(requests[i]->flags_ & MPI_REQ_PREPARED) &&
          not(requests[i]->flags_ & MPI_REQ_FINISHED)) {
        if (requests[i]->action_ != nullptr) {
          XBT_DEBUG("Waiting any %p ", requests[i]);
          comms.push_back(requests[i]->action_.get());
          map.push_back(i);
        } else {
          // This is a finished detached request, let's return this one
          comms.clear(); // don't do the waitany call afterwards
          index = i;
          if (requests[index]->flags_ & MPI_REQ_NBC)
            finish_nbc_requests(&requests[index], 0);
          finish_wait(&requests[i], status); // cleanup if refcount = 0
          if (requests[i] != MPI_REQUEST_NULL && (requests[i]->flags_ & MPI_REQ_NON_PERSISTENT))
            requests[i] = MPI_REQUEST_NULL; // set to null
          break;
        }
      }
    }
    if (not comms.empty()) {
      XBT_DEBUG("Enter waitany for %zu comms", comms.size());
      ssize_t i;
      try{
        kernel::actor::ActorImpl* issuer = kernel::actor::ActorImpl::self();
        simgrid::smpi::ActorExt* process = smpi_process_remote(simgrid::s4u::Actor::by_pid(issuer->get_pid()));
        kernel::actor::ActivityWaitanySimcall observer{issuer, comms, -1,
                                                       process->call_location()->get_call_location()};
        i = kernel::actor::simcall_blocking(
            [&observer] {
              kernel::activity::ActivityImpl::wait_any_for(observer.get_issuer(), observer.get_activities(),
                                                           observer.get_timeout());
            },
            &observer);
      } catch (const CancelException&) {
        XBT_INFO("request cancelled");
        i = -1;
      }

      // not MPI_UNDEFINED, as this is a simix return code
      if (i != -1) {
        index = map[i];
        //in case of an accumulate, we have to wait the end of all requests to apply the operation, ordered correctly.
        if ((requests[index] == MPI_REQUEST_NULL) ||
            (not((requests[index]->flags_ & MPI_REQ_ACCUMULATE) && (requests[index]->flags_ & MPI_REQ_RECV)))) {
          finish_wait(&requests[index],status);
          if (requests[index] != MPI_REQUEST_NULL && (requests[index]->flags_ & MPI_REQ_NON_PERSISTENT))
            requests[index] = MPI_REQUEST_NULL;
        }
      }
    }
  }


  if (index==MPI_UNDEFINED)
    Status::empty(status);

  return index;
}

static int sort_accumulates(const Request* a, const Request* b)
{
  return (a->tag() > b->tag());
}

int Request::waitall(int count, MPI_Request requests[], MPI_Status status[])
{
  std::vector<MPI_Request> accumulates;
  int index;
  MPI_Status stat;
  MPI_Status *pstat = (status == MPI_STATUSES_IGNORE ? MPI_STATUS_IGNORE : &stat);
  int retvalue = MPI_SUCCESS;
  //tag invalid requests in the set
  if (status != MPI_STATUSES_IGNORE) {
    for (int c = 0; c < count; c++) {
      if (requests[c] == MPI_REQUEST_NULL || requests[c]->dst_ == MPI_PROC_NULL ||
          (requests[c]->flags_ & MPI_REQ_PREPARED)) {
        Status::empty(&status[c]);
      } else if (requests[c]->src_ == MPI_PROC_NULL) {
        Status::empty(&status[c]);
        status[c].MPI_SOURCE = MPI_PROC_NULL;
      }
    }
  }
  for (int c = 0; c < count; c++) {
    if (MC_is_active() || MC_record_replay_is_active()) {
      wait(&requests[c],pstat);
      index = c;
    } else {
      index = waitany(count, requests, pstat);

      if (index == MPI_UNDEFINED)
        break;

      if (requests[index] != MPI_REQUEST_NULL && (requests[index]->flags_ & MPI_REQ_RECV) &&
          (requests[index]->flags_ & MPI_REQ_ACCUMULATE))
        accumulates.push_back(requests[index]);
      if (requests[index] != MPI_REQUEST_NULL && (requests[index]->flags_ & MPI_REQ_NON_PERSISTENT))
        requests[index] = MPI_REQUEST_NULL;
    }
    if (status != MPI_STATUSES_IGNORE) {
      status[index] = *pstat;
      if (status[index].MPI_ERROR == MPI_ERR_TRUNCATE)
        retvalue = MPI_ERR_IN_STATUS;
    }
  }

  std::sort(accumulates.begin(), accumulates.end(), sort_accumulates);
  for (auto& req : accumulates)
    finish_wait(&req, status);

  return retvalue;
}

int Request::waitsome(int incount, MPI_Request requests[], int *indices, MPI_Status status[])
{
  int count = 0;
  int flag = 0;
  int index = 0;
  MPI_Status stat;
  MPI_Status *pstat = status == MPI_STATUSES_IGNORE ? MPI_STATUS_IGNORE : &stat;
  index             = waitany(incount, requests, pstat);
  if(index==MPI_UNDEFINED) return MPI_UNDEFINED;
  if(status != MPI_STATUSES_IGNORE) {
    status[count] = *pstat;
  }
  indices[count] = index;
  count++;
  for (int i = 0; i < incount; i++) {
    if (i != index && requests[i] != MPI_REQUEST_NULL && not(requests[i]->flags_ & MPI_REQ_FINISHED)) {
      test(&requests[i], pstat,&flag);
      if (flag==1){
        indices[count] = i;
        if(status != MPI_STATUSES_IGNORE) {
          status[count] = *pstat;
        }
        if (requests[i] != MPI_REQUEST_NULL && (requests[i]->flags_ & MPI_REQ_NON_PERSISTENT))
          requests[i]=MPI_REQUEST_NULL;
        count++;
      }
    }
  }
  return count;
}

MPI_Request Request::f2c(int id)
{
  if(id==MPI_FORTRAN_REQUEST_NULL)
    return MPI_REQUEST_NULL;
  return static_cast<MPI_Request>(F2C::lookup()->at(id));
}

void Request::free_f(int id)
{
  if (id != MPI_FORTRAN_REQUEST_NULL) {
    F2C::lookup()->erase(id);
  }
}

int Request::get_status(const Request* req, int* flag, MPI_Status* status)
{
  if(req != MPI_REQUEST_NULL && req->action_ != nullptr) {
    req->iprobe(req->comm_->group()->rank(req->src_), req->tag_, req->comm_, flag, status);
    if(*flag)
      return MPI_SUCCESS;
  }
  if (req != MPI_REQUEST_NULL && (req->flags_ & MPI_REQ_GENERALIZED) && not(req->flags_ & MPI_REQ_COMPLETE)) {
    *flag = 0;
    return MPI_SUCCESS;
  }

  *flag=1;
  if(req != MPI_REQUEST_NULL &&
     status != MPI_STATUS_IGNORE) {
    aid_t src          = req->src_ == MPI_ANY_SOURCE ? req->real_src_ : req->src_;
    status->MPI_SOURCE = req->comm_->group()->rank(src);
    status->MPI_TAG = req->tag_ == MPI_ANY_TAG ? req->real_tag_ : req->tag_;
    status->MPI_ERROR = req->truncated_ ? MPI_ERR_TRUNCATE : MPI_SUCCESS;
    status->count = req->real_size_;
  }
  return MPI_SUCCESS;
}

int Request::grequest_start(MPI_Grequest_query_function* query_fn, MPI_Grequest_free_function* free_fn,
                            MPI_Grequest_cancel_function* cancel_fn, void* extra_state, MPI_Request* request)
{
  *request = new Request();
  (*request)->flags_ |= MPI_REQ_GENERALIZED;
  (*request)->flags_ |= MPI_REQ_PERSISTENT;
  (*request)->refcount_ = 1;
  ((*request)->generalized_funcs)             = std::make_unique<smpi_mpi_generalized_request_funcs_t>();
  ((*request)->generalized_funcs)->query_fn=query_fn;
  ((*request)->generalized_funcs)->free_fn=free_fn;
  ((*request)->generalized_funcs)->cancel_fn=cancel_fn;
  ((*request)->generalized_funcs)->extra_state=extra_state;
  ((*request)->generalized_funcs)->cond = simgrid::s4u::ConditionVariable::create();
  ((*request)->generalized_funcs)->mutex = simgrid::s4u::Mutex::create();
  return MPI_SUCCESS;
}

int Request::grequest_complete(MPI_Request request)
{
  if ((not(request->flags_ & MPI_REQ_GENERALIZED)) || request->generalized_funcs->mutex == nullptr)
    return MPI_ERR_REQUEST;
  const std::scoped_lock lock(*request->generalized_funcs->mutex);
  request->flags_ |= MPI_REQ_COMPLETE; // in case wait would be called after complete
  request->generalized_funcs->cond->notify_one();
  return MPI_SUCCESS;
}

void Request::start_nbc_requests(std::vector<MPI_Request> reqs){
  if (not reqs.empty()) {
    nbc_requests_ = reqs;
    Request::startall(reqs.size(), reqs.data());
  }
}

std::vector<MPI_Request> Request::get_nbc_requests() const
{
  return nbc_requests_;
}
} // namespace simgrid::smpi
