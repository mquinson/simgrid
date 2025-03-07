/* Copyright (c) 2007-2025. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <simgrid/modelchecker.h>
#include "smpi_win.hpp"

#include "private.hpp"
#include "smpi_coll.hpp"
#include "smpi_comm.hpp"
#include "smpi_datatype.hpp"
#include "smpi_info.hpp"
#include "smpi_keyvals.hpp"
#include "smpi_request.hpp"
#include "src/smpi/include/smpi_actor.hpp"
#include "src/mc/mc_replay.hpp"

#include <algorithm>
#include <mutex> // std::scoped_lock

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(smpi_rma, smpi, "Logging specific to SMPI (RMA operations)");

#define CHECK_RMA_REMOTE_WIN(fun, win)\
  if(target_count*target_datatype->get_extent()>win->size_){\
    XBT_WARN("%s: Trying to move %zd, which exceeds the window size on target process %d : %zd - Bailing out.",\
    fun, target_count*target_datatype->get_extent(), target_rank, win->size_);\
    simgrid::smpi::utils::set_current_buffer(1,"win_base",win->base_);\
    return MPI_ERR_RMA_RANGE;\
  }

#define CHECK_WIN_LOCKED(win)                                                                                          \
  if (opened_ == 0) { /*check that post/start has been done*/                                                          \
    bool locked = std::any_of(begin(win->lockers_), end(win->lockers_), [this](int it) { return it == this->rank_; }); \
    if (not locked)                                                                                                    \
      return MPI_ERR_WIN;                                                                                              \
  }

namespace simgrid::smpi {
std::unordered_map<int, smpi_key_elem> Win::keyvals_;
int Win::keyval_id_=0;

Win::Win(void* base, MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, bool allocated, bool dynamic)
    : base_(base)
    , size_(size)
    , disp_unit_(disp_unit)
    , info_(info)
    , comm_(comm)
    , connected_wins_(comm->size())
    , rank_(comm->rank())
    , allocated_(allocated)
    , dynamic_(dynamic)
{
  XBT_DEBUG("Creating window");
  if(info!=MPI_INFO_NULL)
    info->ref();
  connected_wins_[rank_] = this;
  errhandler_->ref();
  comm->add_rma_win(this);
  comm->ref();

  colls::allgather(&connected_wins_[rank_], sizeof(MPI_Win), MPI_BYTE, connected_wins_.data(), sizeof(MPI_Win),
                   MPI_BYTE, comm);
  if  (MC_is_active() || MC_record_replay_is_active()){
    s4u::Barrier* bar_ptr;
    if (rank_ == 0) {
      bar_ = s4u::Barrier::create(comm->size());
      bar_ptr = bar_.get();
    }
    colls::bcast(&bar_ptr, sizeof(s4u::Barrier*), MPI_BYTE, 0, comm);
    if (rank_ != 0)
      bar_ = s4u::BarrierPtr(bar_ptr);
  }
  this->add_f();
}

int Win::del(Win* win){
  //As per the standard, perform a barrier to ensure every async comm is finished
  if  (MC_is_active() || MC_record_replay_is_active())
    win->bar_->wait();
  else
    colls::barrier(win->comm_);
  win->flush_local_all();

  if (win->info_ != MPI_INFO_NULL)
    simgrid::smpi::Info::unref(win->info_);
  if (win->errhandler_ != MPI_ERRHANDLER_NULL)
    simgrid::smpi::Errhandler::unref(win->errhandler_);

  win->comm_->remove_rma_win(win);

  colls::barrier(win->comm_);
  Comm::unref(win->comm_);
  if (not win->lockers_.empty() || win->opened_ < 0) {
    XBT_WARN("Freeing a locked or opened window");
    return MPI_ERR_WIN;
  }
  if (win->allocated_)
    xbt_free(win->base_);
  for (auto m : {win->mut_, win->lock_mut_, win->atomic_mut_})
    if (m->get_owner() != nullptr)
      m->unlock();

  F2C::free_f(win->f2c_id());
  win->cleanup_attr<Win>();

  delete win;
  return MPI_SUCCESS;
}

int Win::attach(void* /*base*/, MPI_Aint size)
{
  if (not(base_ == MPI_BOTTOM || base_ == nullptr))
    return MPI_ERR_ARG;
  base_ = nullptr; // actually the address will be given in the RMA calls, as being the disp.
  size_+=size;
  return MPI_SUCCESS;
}

int Win::detach(const void* /*base*/)
{
  base_=MPI_BOTTOM;
  size_=-1;
  return MPI_SUCCESS;
}

void Win::get_name(char* name, int* length) const
{
  *length = static_cast<int>(name_.length());
  if (not name_.empty()) {
    name_.copy(name, *length);
    name[*length] = '\0';
  }
}

void Win::get_group(MPI_Group* group){
  if(comm_ != MPI_COMM_NULL){
    *group = comm_->group();
  } else {
    *group = MPI_GROUP_NULL;
  }
}

MPI_Info Win::info()
{
  return info_;
}

int Win::rank() const
{
  return rank_;
}

MPI_Comm Win::comm() const
{
  return comm_;
}

MPI_Aint Win::size() const
{
  return size_;
}

void* Win::base() const
{
  return base_;
}

int Win::disp_unit() const
{
  return disp_unit_;
}

bool Win::dynamic() const
{
  return dynamic_;
}

void Win::set_info(MPI_Info info)
{
  if (info_ != MPI_INFO_NULL)
    simgrid::smpi::Info::unref(info_);
  info_ = info;
  if (info_ != MPI_INFO_NULL)
    info_->ref();
}

void Win::set_name(const char* name){
  name_ = name;
}

int Win::fence(int assert)
{
  XBT_DEBUG("Entering fence");
  opened_++;
  if (not (assert & MPI_MODE_NOPRECEDE)) {
    // This is not the first fence => finalize what came before
    if (MC_is_active() || MC_record_replay_is_active())
      bar_->wait();
    else
      colls::barrier(comm_);
    flush_local_all();
    count_=0;
  }

  if (assert & MPI_MODE_NOSUCCEED) // there should be no ops after this one, tell we are closed.
    opened_=0;
  assert_ = assert;
  if (MC_is_active() || MC_record_replay_is_active())
    bar_->wait();
  else
    colls::barrier(comm_);
  XBT_DEBUG("Leaving fence");

  return MPI_SUCCESS;
}

int Win::put(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank,
              MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Request* request)
{
  //get receiver pointer
  Win* recv_win = connected_wins_[target_rank];

  CHECK_WIN_LOCKED(recv_win)
  CHECK_RMA_REMOTE_WIN("MPI_Put", recv_win)

  void* recv_addr = static_cast<char*>(recv_win->base_) + target_disp * recv_win->disp_unit_;

  if (target_rank != rank_) { // This is not for myself, so we need to send messages
    XBT_DEBUG("Entering MPI_Put to remote rank %d", target_rank);
    // prepare send_request
    MPI_Request sreq =
        Request::rma_send_init(origin_addr, origin_count, origin_datatype, rank_, target_rank, SMPI_RMA_TAG + 1, comm_,
                               MPI_OP_NULL);

    //prepare receiver request
    MPI_Request rreq = Request::rma_recv_init(recv_addr, target_count, target_datatype, rank_, target_rank,
                                              SMPI_RMA_TAG + 1, recv_win->comm_, MPI_OP_NULL);

    //start send
    sreq->start();

    if(request!=nullptr){
      *request=sreq;
    }else{
      const std::scoped_lock lock(*mut_);
      requests_.push_back(sreq);
    }

    //push request to receiver's win
    const std::scoped_lock recv_lock(*recv_win->mut_);
    recv_win->requests_.push_back(rreq);
    rreq->start();
  } else {
    XBT_DEBUG("Entering MPI_Put from myself to myself, rank %d", target_rank);
    Datatype::copy(origin_addr, origin_count, origin_datatype, recv_addr, target_count, target_datatype);
    if(request!=nullptr)
      *request = MPI_REQUEST_NULL;
  }

  return MPI_SUCCESS;
}

int Win::get( void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank,
              MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Request* request)
{
  //get sender pointer
  Win* send_win = connected_wins_[target_rank];

  CHECK_WIN_LOCKED(send_win)
  CHECK_RMA_REMOTE_WIN("MPI_Get", send_win)

  const void* send_addr = static_cast<void*>(static_cast<char*>(send_win->base_) + target_disp * send_win->disp_unit_);
  XBT_DEBUG("Entering MPI_Get from %d", target_rank);

  if (target_rank != rank_) {
    //prepare send_request
    MPI_Request sreq = Request::rma_send_init(send_addr, target_count, target_datatype, target_rank, rank_,
                                              SMPI_RMA_TAG + 2, send_win->comm_, MPI_OP_NULL);

    //prepare receiver request
    MPI_Request rreq = Request::rma_recv_init(origin_addr, origin_count, origin_datatype, target_rank, rank_,
                                              SMPI_RMA_TAG + 2, comm_, MPI_OP_NULL);

    //start the send, with another process than us as sender.
    sreq->start();
    // push request to sender's win
    if (const std::scoped_lock send_lock(*send_win->mut_); true) {
      send_win->requests_.push_back(sreq);
    }

    //start recv
    rreq->start();

    if(request!=nullptr){
      *request=rreq;
    }else{
      const std::scoped_lock lock(*mut_);
      requests_.push_back(rreq);
    }
  } else {
    Datatype::copy(send_addr, target_count, target_datatype, origin_addr, origin_count, origin_datatype);
    if(request!=nullptr)
      *request=MPI_REQUEST_NULL;
  }
  return MPI_SUCCESS;
}

int Win::accumulate(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype, int target_rank,
              MPI_Aint target_disp, int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Request* request)
{
  XBT_DEBUG("Entering MPI_Win_Accumulate");
  //get receiver pointer
  Win* recv_win = connected_wins_[target_rank];

  //FIXME: local version
  CHECK_WIN_LOCKED(recv_win)
  CHECK_RMA_REMOTE_WIN("MPI_Accumulate", recv_win)

  void* recv_addr = static_cast<char*>(recv_win->base_) + target_disp * recv_win->disp_unit_;
  XBT_DEBUG("Entering MPI_Accumulate to %d", target_rank);
  // As the tag will be used for ordering of the operations, subtract count from it (to avoid collisions with other
  // SMPI tags, SMPI_RMA_TAG is set below all the other ones we use)
  // prepare send_request

  MPI_Request sreq = Request::rma_send_init(origin_addr, origin_count, origin_datatype, rank_, target_rank,
                                            SMPI_RMA_TAG - 3 - count_, comm_, op);

  // prepare receiver request
  MPI_Request rreq = Request::rma_recv_init(recv_addr, target_count, target_datatype, rank_, target_rank,
                                            SMPI_RMA_TAG - 3 - count_, recv_win->comm_, op);

  count_++;

  // start send
  sreq->start();
  // push request to receiver's win
  if (const std::scoped_lock recv_lock(*recv_win->mut_); true) {
    recv_win->requests_.push_back(rreq);
    rreq->start();
  }

  if (request != nullptr) {
    *request = sreq;
  } else {
    const std::scoped_lock lock(*mut_);
    requests_.push_back(sreq);
  }

  // FIXME: The current implementation fails to ensure the correct ordering of the accumulate requests.  The following
  // 'flush' is a workaround to fix that.
  flush(target_rank);
  XBT_DEBUG("Leaving MPI_Win_Accumulate");
  return MPI_SUCCESS;
}

int Win::get_accumulate(const void* origin_addr, int origin_count, MPI_Datatype origin_datatype, void* result_addr,
                        int result_count, MPI_Datatype result_datatype, int target_rank, MPI_Aint target_disp,
                        int target_count, MPI_Datatype target_datatype, MPI_Op op, MPI_Request*)
{
  //get sender pointer
  const Win* send_win = connected_wins_[target_rank];

  CHECK_WIN_LOCKED(send_win)
  CHECK_RMA_REMOTE_WIN("MPI_Get_Accumulate", send_win)

  XBT_DEBUG("Entering MPI_Get_accumulate from %d", target_rank);
  //need to be sure ops are correctly ordered, so finish request here ? slow.
  MPI_Request req = MPI_REQUEST_NULL;
  const std::scoped_lock lock(*send_win->atomic_mut_);
  get(result_addr, result_count, result_datatype, target_rank,
              target_disp, target_count, target_datatype, &req);
  if (req != MPI_REQUEST_NULL)
    Request::wait(&req, MPI_STATUS_IGNORE);
  if(op!=MPI_NO_OP)
    accumulate(origin_addr, origin_count, origin_datatype, target_rank,
              target_disp, target_count, target_datatype, op, &req);
  if (req != MPI_REQUEST_NULL)
    Request::wait(&req, MPI_STATUS_IGNORE);
  return MPI_SUCCESS;
}

int Win::compare_and_swap(const void* origin_addr, const void* compare_addr, void* result_addr, MPI_Datatype datatype,
                          int target_rank, MPI_Aint target_disp)
{
  //get sender pointer
  const Win* send_win = connected_wins_[target_rank];

  CHECK_WIN_LOCKED(send_win)

  XBT_DEBUG("Entering MPI_Compare_and_swap with %d", target_rank);
  MPI_Request req = MPI_REQUEST_NULL;
  const std::scoped_lock lock(*send_win->atomic_mut_);
  get(result_addr, 1, datatype, target_rank,
              target_disp, 1, datatype, &req);
  if (req != MPI_REQUEST_NULL)
    Request::wait(&req, MPI_STATUS_IGNORE);
  if (not memcmp(result_addr, compare_addr, datatype->get_extent())) {
    put(origin_addr, 1, datatype, target_rank,
              target_disp, 1, datatype);
  }
  return MPI_SUCCESS;
}

int Win::start(MPI_Group group, int /*assert*/)
{
  /* From MPI forum advices
  The call to MPI_WIN_COMPLETE does not return until the put call has completed at the origin; and the target window
  will be accessed by the put operation only after the call to MPI_WIN_START has matched a call to MPI_WIN_POST by
  the target process. This still leaves much choice to implementors. The call to MPI_WIN_START can block until the
  matching call to MPI_WIN_POST occurs at all target processes. One can also have implementations where the call to
  MPI_WIN_START is nonblocking, but the call to MPI_PUT blocks until the matching call to MPI_WIN_POST occurred; or
  implementations where the first two calls are nonblocking, but the call to MPI_WIN_COMPLETE blocks until the call
  to MPI_WIN_POST occurred; or even implementations where all three calls can complete before any target process
  called MPI_WIN_POST --- the data put must be buffered, in this last case, so as to allow the put to complete at the
  origin ahead of its completion at the target. However, once the call to MPI_WIN_POST is issued, the sequence above
  must complete, without further dependencies.  */

  //naive, blocking implementation.
  XBT_DEBUG("Entering MPI_Win_Start");
  std::vector<MPI_Request> reqs;
  for (int i = 0; i < group->size(); i++) {
    int src = comm_->group()->rank(group->actor(i));
    xbt_assert(src != MPI_UNDEFINED);
    if (src != rank_)
      reqs.emplace_back(Request::irecv_init(nullptr, 0, MPI_CHAR, src, SMPI_RMA_TAG + 4, comm_));
  }
  int size = static_cast<int>(reqs.size());

  Request::startall(size, reqs.data());
  Request::waitall(size, reqs.data(), MPI_STATUSES_IGNORE);
  for (auto& req : reqs)
    Request::unref(&req);

  group->ref();
  dst_group_ = group;
  opened_--; // we're open for business !
  XBT_DEBUG("Leaving MPI_Win_Start");
  return MPI_SUCCESS;
}

int Win::post(MPI_Group group, int /*assert*/)
{
  //let's make a synchronous send here
  XBT_DEBUG("Entering MPI_Win_Post");
  std::vector<MPI_Request> reqs;
  for (int i = 0; i < group->size(); i++) {
    int dst = comm_->group()->rank(group->actor(i));
    xbt_assert(dst != MPI_UNDEFINED);
    if (dst != rank_)
      reqs.emplace_back(Request::send_init(nullptr, 0, MPI_CHAR, dst, SMPI_RMA_TAG + 4, comm_));
  }
  int size = static_cast<int>(reqs.size());

  Request::startall(size, reqs.data());
  Request::waitall(size, reqs.data(), MPI_STATUSES_IGNORE);
  for (auto& req : reqs)
    Request::unref(&req);

  group->ref();
  src_group_ = group;
  opened_--; // we're open for business !
  XBT_DEBUG("Leaving MPI_Win_Post");
  return MPI_SUCCESS;
}

int Win::complete(){
  xbt_assert(opened_ != 0, "Complete called on already opened MPI_Win");

  XBT_DEBUG("Entering MPI_Win_Complete");
  std::vector<MPI_Request> reqs;
  for (int i = 0; i < dst_group_->size(); i++) {
    int dst = comm_->group()->rank(dst_group_->actor(i));
    xbt_assert(dst != MPI_UNDEFINED);
    if (dst != rank_)
      reqs.emplace_back(Request::send_init(nullptr, 0, MPI_CHAR, dst, SMPI_RMA_TAG + 5, comm_));
  }
  int size = static_cast<int>(reqs.size());

  XBT_DEBUG("Win_complete - Sending sync messages to %d processes", size);
  Request::startall(size, reqs.data());
  Request::waitall(size, reqs.data(), MPI_STATUSES_IGNORE);
  for (auto& req : reqs)
    Request::unref(&req);

  flush_local_all();

  opened_++; //we're closed for business !
  Group::unref(dst_group_);
  dst_group_ = MPI_GROUP_NULL;
  return MPI_SUCCESS;
}

int Win::wait(){
  //naive, blocking implementation.
  XBT_DEBUG("Entering MPI_Win_Wait");
  std::vector<MPI_Request> reqs;
  for (int i = 0; i < src_group_->size(); i++) {
    int src = comm_->group()->rank(src_group_->actor(i));
    xbt_assert(src != MPI_UNDEFINED);
    if (src != rank_)
      reqs.emplace_back(Request::irecv_init(nullptr, 0, MPI_CHAR, src, SMPI_RMA_TAG + 5, comm_));
  }
  int size = static_cast<int>(reqs.size());

  XBT_DEBUG("Win_wait - Receiving sync messages from %d processes", size);
  Request::startall(size, reqs.data());
  Request::waitall(size, reqs.data(), MPI_STATUSES_IGNORE);
  for (auto& req : reqs)
    Request::unref(&req);

  flush_local_all();

  opened_++; //we're closed for business !
  Group::unref(src_group_);
  src_group_ = MPI_GROUP_NULL;
  return MPI_SUCCESS;
}

int Win::lock(int lock_type, int rank, int /*assert*/)
{
  MPI_Win target_win = connected_wins_[rank];

  if ((lock_type == MPI_LOCK_EXCLUSIVE && target_win->mode_ != MPI_LOCK_SHARED)|| target_win->mode_ == MPI_LOCK_EXCLUSIVE){
    target_win->lock_mut_->lock();
    target_win->mode_+= lock_type;//add the lock_type to differentiate case when we are switching from EXCLUSIVE to SHARED (no release needed in the unlock)
    if(lock_type == MPI_LOCK_SHARED){//the window used to be exclusive, it's now shared.
      target_win->lock_mut_->unlock();
   }
  } else if (not(target_win->mode_ == MPI_LOCK_SHARED && lock_type == MPI_LOCK_EXCLUSIVE))
    target_win->mode_ += lock_type; // don't set to exclusive if it's already shared

  target_win->lockers_.push_back(rank_);

  flush(rank);
  return MPI_SUCCESS;
}

int Win::lock_all(int assert){
  int retval = MPI_SUCCESS;
  for (int i = 0; i < comm_->size(); i++) {
    int ret = this->lock(MPI_LOCK_SHARED, i, assert);
    if (ret != MPI_SUCCESS)
      retval = ret;
  }
  return retval;
}

int Win::unlock(int rank){
  MPI_Win target_win = connected_wins_[rank];
  int target_mode = target_win->mode_;
  target_win->mode_= 0;
  target_win->lockers_.remove(rank_);
  if (target_mode==MPI_LOCK_EXCLUSIVE){
    target_win->lock_mut_->unlock();
  }

  flush(rank);
  return MPI_SUCCESS;
}

int Win::unlock_all(){
  int retval = MPI_SUCCESS;
  for (int i = 0; i < comm_->size(); i++) {
    int ret = this->unlock(i);
    if (ret != MPI_SUCCESS)
      retval = ret;
  }
  return retval;
}

int Win::flush(int rank){
  int finished = finish_comms(rank);
  XBT_DEBUG("Win_flush on local %d for remote %d - Finished %d RMA calls", rank_, rank, finished);
  if (rank != rank_) {
    finished = connected_wins_[rank]->finish_comms(rank_);
    XBT_DEBUG("Win_flush on remote %d for local %d - Finished %d RMA calls", rank, rank_, finished);
  }
  return MPI_SUCCESS;
}

int Win::flush_local(int rank){
  int finished = finish_comms(rank);
  XBT_DEBUG("Win_flush_local on local %d for remote %d - Finished %d RMA calls", rank_, rank, finished);
  return MPI_SUCCESS;
}

int Win::flush_all(){
  int finished = finish_comms();
  XBT_DEBUG("Win_flush_all on local %d - Finished %d RMA calls", rank_, finished);
  for (int i = 0; i < comm_->size(); i++) {
    if (i != rank_) {
      finished = connected_wins_[i]->finish_comms(rank_);
      XBT_DEBUG("Win_flush_all on remote %d for local %d - Finished %d RMA calls", i, rank_, finished);
    }
  }
  return MPI_SUCCESS;
}

int Win::flush_local_all(){
  int finished = finish_comms();
  XBT_DEBUG("Win_flush_local_all on local %d - Finished %d RMA calls", rank_, finished);
  return MPI_SUCCESS;
}

Win* Win::f2c(int id){
  return static_cast<Win*>(F2C::f2c(id));
}

int Win::finish_comms(){
  // This (simulated) mutex ensures that no process pushes to the vector of requests during the waitall.
  // Without this, the vector could get redimensioned when another process pushes.
  // This would result in the array used by Request::waitall() to be invalidated.
  // Another solution would be to copy the data and cleanup the vector *before* Request::waitall
  const std::scoped_lock lock(*mut_);
  //Finish own requests
  int size = static_cast<int>(requests_.size());
  if (size > 0) {
    MPI_Request* treqs = requests_.data();
    Request::waitall(size, treqs, MPI_STATUSES_IGNORE);
    requests_.clear();
  }
  return size;
}

int Win::finish_comms(int rank){
  // See comment about the mutex in finish_comms() above
  const std::scoped_lock lock(*mut_);
  // Finish own requests
  // Let's see if we're either the destination or the sender of this request
  // because we only wait for requests that we are responsible for.
  // Also use the process id here since the request itself returns from src()
  // and dst() the process id, NOT the rank (which only exists in the context of a communicator).
  aid_t proc_id = comm_->group()->actor(rank);
  auto it     = std::stable_partition(begin(requests_), end(requests_), [proc_id](const MPI_Request& req) {
    return (req == MPI_REQUEST_NULL || (req->src() != proc_id && req->dst() != proc_id));
  });
  std::vector<MPI_Request> myreqqs(it, end(requests_));
  requests_.erase(it, end(requests_));
  int size = static_cast<int>(myreqqs.size());
  if (size > 0) {
    MPI_Request* treqs = myreqqs.data();
    Request::waitall(size, treqs, MPI_STATUSES_IGNORE);
    myreqqs.clear();
  }
  return size;
}

int Win::shared_query(int rank, MPI_Aint* size, int* disp_unit, void* baseptr) const
{
  const Win* target_win = rank != MPI_PROC_NULL ? connected_wins_[rank] : nullptr;
  for (int i = 0; not target_win && i < comm_->size(); i++) {
    if (connected_wins_[i]->size_ > 0)
      target_win = connected_wins_[i];
  }
  if (target_win) {
    *size                         = target_win->size_;
    *disp_unit                    = target_win->disp_unit_;
    *static_cast<void**>(baseptr) = target_win->base_;
  } else {
    *size                         = 0;
    *static_cast<void**>(baseptr) = nullptr;
  }
  return MPI_SUCCESS;
}

MPI_Errhandler Win::errhandler()
{
  if (errhandler_ != MPI_ERRHANDLER_NULL)
    errhandler_->ref();
  return errhandler_;
}

void Win::set_errhandler(MPI_Errhandler errhandler)
{
  if (errhandler_ != MPI_ERRHANDLER_NULL)
    simgrid::smpi::Errhandler::unref(errhandler_);
  errhandler_ = errhandler;
  if (errhandler_ != MPI_ERRHANDLER_NULL)
    errhandler_->ref();
}
} // namespace simgrid::smpi
