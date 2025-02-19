/* Copyright (c) 2015-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/remote/AppSide.hpp"
#include "simgrid/s4u/Host.hpp"
#include "src/internal_config.h"
#include "src/kernel/EngineImpl.hpp"
#include "src/kernel/actor/ActorImpl.hpp"
#include "src/kernel/actor/SimcallObserver.hpp"
#include "src/mc/mc_base.hpp"
#include "src/mc/mc_config.hpp"
#include "src/mc/mc_environ.h"
#include "xbt/asserts.h"
#include "xbt/log.h"
#include <cstddef>
#include <sstream>
#if HAVE_SMPI
#include "src/smpi/include/private.hpp"
#endif
#include "src/sthread/sthread.h"
#include "src/xbt/coverage.h"
#include "xbt/str.h"
#include <simgrid/modelchecker.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio> // setvbuf
#include <cstdlib>
#include <memory>
#include <numeric>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_client, mc, "MC client logic");
XBT_LOG_EXTERNAL_CATEGORY(mc_global);

namespace simgrid::mc {

std::unique_ptr<AppSide> AppSide::instance_;

AppSide* AppSide::get()
{
  // Only initialize the MC world once
  if (instance_ != nullptr)
    return instance_.get();

  if (std::getenv(MC_ENV_SOCKET_FD) == nullptr) // We are not in MC mode: don't initialize the MC world
    return nullptr;

  XBT_DEBUG("Initialize the MC world.");

  simgrid::mc::set_model_checking_mode(ModelCheckingMode::APP_SIDE);

  setvbuf(stdout, nullptr, _IOLBF, 0);

  // Fetch socket from MC_ENV_SOCKET_FD:
  const char* fd_env = std::getenv(MC_ENV_SOCKET_FD);
  int fd             = xbt_str_parse_int(fd_env, "Not a number in variable '" MC_ENV_SOCKET_FD "'");
  XBT_DEBUG("Model-checked application found socket FD %i", fd);

  instance_ = std::make_unique<simgrid::mc::AppSide>(fd);

  // If we plan to fork, remove the SIGINT handler that would get messed up by all the forked childs
  if (not _sg_mc_nofork)
    std::signal(SIGINT, SIG_DFL);

  instance_->handle_messages();
  return instance_.get();
}

void AppSide::handle_deadlock_check(const s_mc_message_int_t* request) const
{
  const auto* engine     = kernel::EngineImpl::get_instance();
  const auto& actor_list = engine->get_actor_list();
  bool deadlock = not actor_list.empty() && std::none_of(begin(actor_list), end(actor_list), [](const auto& kv) {
    return mc::actor_is_enabled(kv.second);
  });

  if (deadlock && request->value) {
    XBT_CINFO(mc_global, "**************************");
    XBT_CINFO(mc_global, "*** DEADLOCK DETECTED ***");
    XBT_CINFO(mc_global, "**************************");
    engine->display_all_actor_status();
  }
  // Send result:
  s_mc_message_int_t answer = {};
  answer.type  = MessageType::DEADLOCK_CHECK_REPLY;
  answer.value = deadlock;
  xbt_assert(channel_.send(answer) == 0, "Could not send response: %s", strerror(errno));
}
void AppSide::handle_simcall_execute(const s_mc_message_simcall_execute_t* message) const
{
  kernel::actor::ActorImpl* actor = kernel::EngineImpl::get_instance()->get_actor_by_pid(message->aid_);
  xbt_assert(actor != nullptr, "Invalid pid %ld", message->aid_);
  xbt_assert(
      (actor->simcall_.observer_ == nullptr && actor->simcall_.call_ != simgrid::kernel::actor::Simcall::Type::NONE) ||
          (actor->simcall_.observer_ != nullptr && actor->simcall_.observer_->is_enabled()),
      "Please, model-checker, don't execute disabled transitions. You tried to execute %s which is disabled",
      actor->simcall_.observer_->to_string().c_str());

  // The client may send some messages to the server while processing the transition
  actor->simcall_handle(message->times_considered_);
  // Say the server that the transition is over and that it should proceed
  xbt_assert(channel_.send(MessageType::WAITING) == 0, "Could not send MESSAGE_WAITING to model-checker: %s",
             strerror(errno));

  // Finish the RPC from the server: return a serialized observer, to build a Transition on Checker side
  s_mc_message_simcall_execute_answer_t answer;
  answer.type = MessageType::SIMCALL_EXECUTE_REPLY;
  answer.aid  = actor->get_pid();
  if (message->want_transition) {

    std::stringstream stream;
    if (actor->simcall_.observer_ != nullptr) {
      actor->simcall_.observer_->serialize(stream);
    } else {
      stream << (short)mc::Transition::Type::UNKNOWN;
    }
    std::string str = stream.str();
    xbt_assert(str.size() + 1 <= answer.buffer.size(),
               "The serialized simcall is too large for the buffer. Please fix the code.");
    strncpy(answer.buffer.data(), str.c_str(), answer.buffer.size() - 1);
    answer.buffer.back() = '\0';
    XBT_VERB("send SIMCALL_EXECUTE_ANSWER(%s) ~> '%s'", actor->get_cname(), str.c_str());
  } else
    XBT_VERB("send SIMCALL_EXECUTE_ANSWER(%s) with no transition inside", actor->get_cname());

  xbt_assert(channel_.send(answer) == 0, "Could not send response: %s", strerror(errno));
}

void AppSide::handle_replay(const s_mc_message_replay_t* msg) const
{
  aid_t aid;
  int times_considered;
  int replay_size = msg->count;
  XBT_DEBUG("Going to replay %d transitions", replay_size);
  for (int i = 0; i < replay_size; i++) {
    aid              = msg->aids[i];
    times_considered = msg->times[i];

    XBT_VERB("MC asked to replay %ld(nb_times=%d)", aid, times_considered);
    kernel::actor::ActorImpl* actor = kernel::EngineImpl::get_instance()->get_actor_by_pid(aid);
    xbt_assert(actor != nullptr, "Invalid pid %ld", aid);
    xbt_assert((actor->simcall_.observer_ == nullptr &&
                actor->simcall_.call_ != simgrid::kernel::actor::Simcall::Type::NONE) ||
                   (actor->simcall_.observer_ != nullptr && actor->simcall_.observer_->is_enabled()),
               "Please, model-checker, don't execute disabled transitions. You tried to execute %s which is disabled",
               actor->simcall_.observer_->to_string().c_str());

    actor->simcall_handle(times_considered);
    simgrid::mc::execute_actors();
  }

  // Say the server that the replay is over and that it should proceed
  xbt_assert(channel_.send(MessageType::WAITING) == 0, "Could not send MESSAGE_WAITING to model-checker: %s",
             strerror(errno));
}

void AppSide::handle_finalize(const s_mc_message_int_t* msg) const
{
  bool terminate_asap = msg->value;
  XBT_DEBUG("Finalize (terminate = %d)", (int)terminate_asap);
  if (not terminate_asap) {
    if (XBT_LOG_ISENABLED(mc_client, xbt_log_priority_debug))
      kernel::EngineImpl::get_instance()->display_all_actor_status();
#if HAVE_SMPI
    XBT_DEBUG("Smpi_enabled: %d", SMPI_is_inited());
    if (SMPI_is_inited())
      SMPI_finalize();
#endif
  }
  coverage_checkpoint();
  xbt_assert(channel_.send(MessageType::FINALIZE_REPLY) == 0, "Could not answer to FINALIZE: %s", strerror(errno));
  std::fflush(stdout);
  if (terminate_asap)
    ::_Exit(0);
}
void AppSide::handle_fork(const s_mc_message_fork_t* msg)
{
  static bool first_time = true;
  if (first_time && std::strcmp(simgrid::s4u::Engine::get_instance()->get_context_factory_name(), "thread") == 0) {
    s_mc_message_int_t answer = {.type = MessageType::FORK_REPLY, .value = 0};
    xbt_assert(channel_.send(answer) == 0, "Could not send failure as a response to FORK: %s", strerror(errno));

    xbt_die("The SimGrid model-checker cannot handle the threaded context factory because it relies on forking the "
            "application, which is not possible with multi-threaded applications. Please remove the "
            "--cfg=contexts/factory:thread parameter to the application, or add --cfg=model-check/no-fork:1 to "
            "simgrid-mc. The second option results in slower explorations, but it is the only option when verifying "
            "Python programs, as the SimGrid Python bindings mandate threads.");
    // See also commit c2c077dc7edb4a4217610dbaf675f414cbf6f09f for the reason why Python needs threads
  }
  first_time = false;

  int status;
  int pid;
  /* Reap any zombie child, saving its status for later use in AppSide::handle_wait_child() */
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    child_statuses_[pid] = status;

  pid = fork();
  xbt_assert(pid >= 0, "Could not fork application sub-process: %s.", strerror(errno));

  if (pid == 0) { // Child
    int sock = socket(AF_UNIX,
#ifdef __APPLE__
                      SOCK_STREAM, /* Mac OSX does not have AF_UNIX + SOCK_SEQPACKET, even if that's faster*/
#else
                      SOCK_SEQPACKET,
#endif
                      0);

    struct sockaddr_un addr = {};
    addr.sun_family         = AF_UNIX;
    std::copy_n(begin(msg->socket_name), MC_SOCKET_NAME_LEN, addr.sun_path);

    xbt_assert(connect(sock, (struct sockaddr*)&addr, sizeof addr) >= 0, "Cannot connect to Checker on %c%s: %s.",
               (addr.sun_path[0] ? addr.sun_path[0] : '@'), addr.sun_path + 1, strerror(errno));

    channel_.reset_socket(sock);

    s_mc_message_int_t answer = {};
    answer.type               = MessageType::FORK_REPLY;
    answer.value              = getpid();
    xbt_assert(channel_.send(answer) == 0, "Could not send response to FORK: %s", strerror(errno));
  } else {
    XBT_VERB("App %d forks subprocess %d.", getpid(), pid);
  }
}
void AppSide::handle_wait_child(const s_mc_message_int_t* msg)
{
  int status;
  errno = 0;
  if (auto search = child_statuses_.find(msg->value); search != child_statuses_.end()) {
    status = search->second;
    child_statuses_.erase(search); // We only need this info once
  } else {
    waitpid(msg->value, &status, 0);
  }
  xbt_assert(errno == 0, "Cannot wait on behalf of the checker: %s.", strerror(errno));

  s_mc_message_int_t answer = {};
  answer.type               = MessageType::WAIT_CHILD_REPLY;
  answer.value              = status;
  xbt_assert(channel_.send(answer) == 0, "Could not send response to WAIT_CHILD: %s", strerror(errno));
}
void AppSide::handle_actors_status(const s_mc_message_actors_status_t* msg) const
{
  auto const& actor_list = kernel::EngineImpl::get_instance()->get_actor_list();
  XBT_DEBUG("Serialize the actors to answer ACTORS_STATUS from the checker. %zu actors to go.", actor_list.size());

  std::vector<s_mc_message_actors_status_one_t> status;
  for (auto const& [aid, actor] : actor_list) {
    xbt_assert(actor);
    xbt_assert(actor->simcall_.observer_, "simcall %s in actor %s has no observer.", actor->simcall_.get_cname(),
               actor->get_cname());
    s_mc_message_actors_status_one_t one = {};
    one.type                             = MessageType::ACTORS_STATUS_REPLY_TRANSITION;
    one.aid                              = aid;
    one.enabled                          = mc::actor_is_enabled(actor);
    one.max_considered                   = actor->simcall_.observer_->get_max_consider();
    status.push_back(one);
  }

  struct s_mc_message_actors_status_answer_t answer = {};
  answer.type                                       = MessageType::ACTORS_STATUS_REPLY_COUNT;
  answer.count                                      = static_cast<int>(status.size());

  xbt_assert(channel_.send(answer) == 0, "Could not send ACTORS_STATUS_REPLY msg: %s", strerror(errno));
  if (answer.count > 0) {
    size_t size = status.size() * sizeof(s_mc_message_actors_status_one_t);
    xbt_assert(channel_.send(status.data(), size) == 0, "Could not send ACTORS_STATUS_REPLY data: %s", strerror(errno));
  }

  if (not msg->want_transitions_)
    return;

  // Serialize each transition to describe what each actor is doing
  XBT_DEBUG("Deliver ACTOR_TRANSITION_PROBE payload");
  for (const auto& actor_status : status) {
    const auto& actor        = actor_list.at(actor_status.aid);
    const int max_considered = actor_status.max_considered;

    for (int times_considered = 0; times_considered < max_considered; times_considered++) {
      std::stringstream stream;
      s_mc_message_simcall_probe_one_t probe;
      probe.type = MessageType::ACTORS_STATUS_REPLY_SIMCALL;

      if (actor->simcall_.observer_ != nullptr) {
        actor->simcall_.observer_->prepare(times_considered);
        actor->simcall_.observer_->serialize(stream);
      } else {
        stream << (short)mc::Transition::Type::UNKNOWN;
      }

      std::string str = stream.str();
      xbt_assert(str.size() + 1 <= probe.buffer.size(),
                 "The serialized transition is too large for the buffer. Please fix the code.");
      strncpy(probe.buffer.data(), str.c_str(), probe.buffer.size() - 1);
      probe.buffer.back() = '\0';

      XBT_VERB("send ACTOR_TRANSITION_PROBE(%s) ~> '%s'", actor->get_cname(), str.c_str());
      xbt_assert(channel_.send(probe) == 0, "Could not send ACTOR_TRANSITION_PROBE payload: %s", strerror(errno));
    }
    // NOTE: We do NOT need to reset `times_considered` for each actor's
    // simcall observer here to the "original" value (i.e. the value BEFORE
    // multiple prepare() calls were made for serialization purposes) since
    // each SIMCALL_EXECUTE provides a `times_considered` to be used to prepare
    // the transition before execution.
  }
}
void AppSide::handle_actors_maxpid() const
{
  s_mc_message_int_t answer = {};
  answer.type               = MessageType::ACTORS_MAXPID_REPLY;
  answer.value              = kernel::actor::ActorImpl::get_maxpid();
  xbt_assert(channel_.send(answer) == 0, "Could not send response: %s", strerror(errno));
}

#define assert_msg_size(_name_, _type_)                                                                                \
  xbt_assert(received_size == sizeof(_type_), "Unexpected size for " _name_ " (%zd != %zu)", received_size,            \
             sizeof(_type_))

void AppSide::handle_messages()
{
  while (true) { // Until we get a CONTINUE message
    XBT_DEBUG("Waiting messages from the model-checker");

    std::array<char, MC_MESSAGE_LENGTH> message_buffer;
    ssize_t received_size = channel_.receive(message_buffer.data(), message_buffer.size());

    if (received_size == 0) {
      XBT_DEBUG("Socket closed on the Checker side, bailing out.");
      ::_Exit(0); // Nobody's listening to that process anymore => exit as quickly as possible.
    }
    xbt_assert(received_size >= 0, "Could not receive commands from the model-checker: %s", strerror(errno));
    xbt_assert(static_cast<size_t>(received_size) >= sizeof(s_mc_message_t), "Cannot handle short message (size=%zd)",
               received_size);

    const s_mc_message_t* message = (s_mc_message_t*)message_buffer.data();
    switch (message->type) {
      case MessageType::CONTINUE:
        assert_msg_size("MESSAGE_CONTINUE", s_mc_message_t);
        return;

      case MessageType::DEADLOCK_CHECK:
        assert_msg_size("DEADLOCK_CHECK", s_mc_message_int_t);
        handle_deadlock_check((s_mc_message_int_t*)message);
        break;

      case MessageType::SIMCALL_EXECUTE:
        assert_msg_size("SIMCALL_EXECUTE", s_mc_message_simcall_execute_t);
        handle_simcall_execute((s_mc_message_simcall_execute_t*)message_buffer.data());
        break;

      case MessageType::REPLAY:
        assert_msg_size("REPLAY", s_mc_message_replay_t);
        handle_replay((s_mc_message_replay_t*)message_buffer.data());
        break;

      case MessageType::FINALIZE:
        assert_msg_size("FINALIZE", s_mc_message_int_t);
        handle_finalize((s_mc_message_int_t*)message_buffer.data());
        break;

      case MessageType::FORK:
        assert_msg_size("FORK", s_mc_message_fork_t);
        handle_fork((s_mc_message_fork_t*)message_buffer.data());
        break;

      case MessageType::WAIT_CHILD:
        assert_msg_size("WAIT_CHILD", s_mc_message_int_t);
        handle_wait_child((s_mc_message_int_t*)message_buffer.data());
        break;

      case MessageType::ACTORS_STATUS:
        assert_msg_size("ACTORS_STATUS", s_mc_message_actors_status_t);
        handle_actors_status((s_mc_message_actors_status_t*)message_buffer.data());
        break;

      case MessageType::ACTORS_MAXPID:
        assert_msg_size("ACTORS_MAXPID", s_mc_message_t);
        handle_actors_maxpid();
        break;

      default:
        xbt_die("Received unexpected message %s (%i)", to_c_str(message->type), static_cast<int>(message->type));
        break;
    }
  }
}

void AppSide::main_loop()
{
  simgrid::mc::processes_time.resize(simgrid::kernel::actor::ActorImpl::get_maxpid());

  sthread_disable();
  coverage_checkpoint();
  sthread_enable();
  while (true) {
    simgrid::mc::execute_actors();
    xbt_assert(channel_.send(MessageType::WAITING) == 0, "Could not send WAITING message to model-checker: %s",
               strerror(errno));
    this->handle_messages();
  }
}

void AppSide::report_assertion_failure()
{
  xbt_assert(channel_.send(MessageType::ASSERTION_FAILED) == 0, "Could not send assertion to model-checker: %s",
             strerror(errno));
  this->handle_messages();
}

} // namespace simgrid::mc
