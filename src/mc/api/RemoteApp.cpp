/* Copyright (c) 2015-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/api/RemoteApp.hpp"
#include "simgrid/forward.h"
#include "src/mc/api/states/State.hpp"
#include "src/mc/explo/Exploration.hpp"
#include "src/mc/mc_config.hpp"
#include "src/mc/mc_exit.hpp"
#include "src/mc/mc_private.hpp"
#include "src/mc/remote/CheckerSide.hpp"
#include "xbt/asserts.h"
#include "xbt/log.h"
#include "xbt/system_error.hpp"
#include <limits>
#include <signal.h>

#include <algorithm>
#include <array>
#include <limits.h>
#include <memory>
#include <numeric>
#include <string>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#ifdef __linux__
#include <sys/personality.h>
#endif

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_Session, mc, "Model-checker session");
XBT_LOG_EXTERNAL_CATEGORY(mc_global);

namespace simgrid::mc {

static std::string master_socket_name;

RemoteApp::RemoteApp(const std::vector<char*>& args) : app_args_(args)
{
  master_socket_ = socket(AF_UNIX,
#ifdef __APPLE__
                          SOCK_STREAM, /* Mac OSX does not have AF_UNIX + SOCK_SEQPACKET, even if that's faster */
#else
                          SOCK_SEQPACKET,
#endif
                          0);
  xbt_assert(master_socket_ != -1, "Cannot create the master socket: %s", strerror(errno));

  master_socket_name = "/tmp/simgrid-mc-" + std::to_string(getpid());
  master_socket_name.resize(MC_SOCKET_NAME_LEN); // truncate socket name if it's too long
  master_socket_name.back() = '\0';              // ensure the data are null-terminated
#ifdef __linux__
  master_socket_name[0] = '\0'; // abstract socket, automatically removed after close
#else
  unlink(master_socket_name.c_str()); // remove possible stale socket before bind
  atexit([]() {
    if (not master_socket_name.empty())
      unlink(master_socket_name.c_str());
    master_socket_name.clear();
  });
#endif

  struct sockaddr_un serv_addr = {};
  serv_addr.sun_family         = AF_UNIX;
  master_socket_name.copy(serv_addr.sun_path, MC_SOCKET_NAME_LEN);

  xbt_assert(bind(master_socket_, (struct sockaddr*)&serv_addr, sizeof serv_addr) >= 0,
             "Cannot bind the master socket to %c%s: %s.", (serv_addr.sun_path[0] ? serv_addr.sun_path[0] : '@'),
             serv_addr.sun_path + 1, strerror(errno));

  xbt_assert(listen(master_socket_, SOMAXCONN) >= 0, "Cannot listen to the master socket: %s.", strerror(errno));

#ifdef __linux__
  // Reduce the randomness under Linux
  personality(ADDR_NO_RANDOMIZE);
#endif

  if (_sg_mc_nofork) {
    checker_side_ = std::make_unique<simgrid::mc::CheckerSide>(app_args_);
  } else {
    application_factory_ = std::make_unique<simgrid::mc::CheckerSide>(app_args_);
    checker_side_        = application_factory_->clone(master_socket_, master_socket_name);
  }
}

void RemoteApp::restore_checker_side(CheckerSide* from, bool finalize_app)
{
  XBT_DEBUG("Restore the checker side");

  if (checker_side_ and finalize_app)
    checker_side_->finalize(true);

  if (_sg_mc_nofork) {
    checker_side_ = std::make_unique<simgrid::mc::CheckerSide>(app_args_);
  } else if (from == nullptr) {
    checker_side_ = application_factory_->clone(master_socket_, master_socket_name);
  } else {
    checker_side_ = from->clone(master_socket_, master_socket_name);
  }
}
std::unique_ptr<CheckerSide> RemoteApp::clone_checker_side()
{
  xbt_assert(not _sg_mc_nofork, "Cannot clone the checker_side in nofork mode");
  XBT_DEBUG("Clone the checker side, saving an intermediate state");
  return checker_side_->clone(master_socket_, master_socket_name);
}

unsigned long RemoteApp::get_maxpid() const
{
  // note: we could maybe cache it and count the actor creation on checker side too.
  // But counting correctly accross state checkpoint/restore would be annoying.

  checker_side_->get_channel().send(MessageType::ACTORS_MAXPID);
  s_mc_message_int_t answer;
  ssize_t answer_size = checker_side_->get_channel().receive(answer);
  xbt_assert(answer_size != -1, "Could not receive message");
  xbt_assert(answer_size == sizeof answer, "Broken message (size=%zd; expected %zu)", answer_size, sizeof answer);
  xbt_assert(answer.type == MessageType::ACTORS_MAXPID_REPLY,
             "Received unexpected message %s (%i); expected MessageType::ACTORS_MAXPID_REPLY (%i)",
             to_c_str(answer.type), (int)answer.type, (int)MessageType::ACTORS_MAXPID_REPLY);

  return answer.value;
}

void RemoteApp::get_actors_status(std::map<aid_t, ActorState>& whereto) const
{
  // The messaging happens as follows:
  //
  // CheckerSide                  AppSide
  // send ACTORS_STATUS ---->
  //                    <----- send ACTORS_STATUS_REPLY_COUNT
  //                    <----- send `N` ACTORS_STATUS_REPLY_TRANSITION (s_mc_message_actors_status_one_t)
  //                    <----- send `M` ACTORS_STATUS_REPLY_SIMCALL (s_mc_message_simcall_probe_one_t)
  //
  // Note that we also receive disabled transitions, because the guiding strategies need them to decide what could
  // unlock actors.

  s_mc_message_restore_t msg = {MessageType::ACTORS_STATUS,
                                Exploration::get_instance()->need_actor_status_transitions()};
  checker_side_->get_channel().send(msg);

  s_mc_message_actors_status_answer_t answer;
  ssize_t answer_size = checker_side_->get_channel().receive(answer);
  xbt_assert(answer_size != -1, "Could not receive message");
  xbt_assert(answer_size == sizeof answer, "Broken message (size=%zd; expected %zu)", answer_size, sizeof answer);
  xbt_assert(answer.type == MessageType::ACTORS_STATUS_REPLY_COUNT,
             "%d Received unexpected message %s (%i); expected MessageType::ACTORS_STATUS_REPLY_COUNT (%i)", getpid(),
             to_c_str(answer.type), (int)answer.type, (int)MessageType::ACTORS_STATUS_REPLY_COUNT);

  // Message sanity checks
  xbt_assert(answer.count >= 0, "Received an ACTORS_STATUS_REPLY_COUNT message with an actor count of '%d' < 0",
             answer.count);
  xbt_assert(answer.count < std::numeric_limits<short>::max(),
             "The applications has more than %d actors, but the model-checker saves aid_t on an "
             "short int to save memory. Such app is probably too big for MC anyway.",
             std::numeric_limits<short>::max());

  std::vector<s_mc_message_actors_status_one_t> status(answer.count);
  if (answer.count > 0) {
    size_t size      = status.size() * sizeof(s_mc_message_actors_status_one_t);
    ssize_t received = checker_side_->get_channel().receive(status.data(), size);
    xbt_assert(static_cast<size_t>(received) == size);
  }

  whereto.clear();

  for (const auto& actor : status) {
    std::vector<std::shared_ptr<Transition>> actor_transitions;

    if (Exploration::get_instance()->need_actor_status_transitions()) {
      int n_transitions = actor.max_considered;
      for (int times_considered = 0; times_considered < n_transitions; times_considered++) {
        s_mc_message_simcall_probe_one_t probe;
        ssize_t received = checker_side_->get_channel().receive(probe);
        xbt_assert(received >= 0, "Could not receive response to ACTORS_PROBE message (%s)", strerror(errno));
        xbt_assert(static_cast<size_t>(received) == sizeof probe,
                   "Could not receive response to ACTORS_PROBE message (%zd bytes received != %zu bytes expected",
                   received, sizeof probe);

        std::stringstream stream(probe.buffer.data());
        if (Exploration::get_instance()->need_actor_status_transitions())
          actor_transitions.emplace_back(deserialize_transition(actor.aid, times_considered, stream));
      }
      XBT_DEBUG("Received %zu transitions for actor %ld. The first one is %s", actor_transitions.size(), actor.aid,
                (actor_transitions.size() > 0 ? actor_transitions[0]->to_string().c_str() : "null"));
    }

    whereto.try_emplace(actor.aid, actor.aid, actor.enabled, actor.max_considered, std::move(actor_transitions));
  }
}

bool RemoteApp::check_deadlock(bool verbose) const
{

  auto* explo = Exploration::get_instance();
  // While looking for critical transition, we don't want to dilute the output with
  // all the deadlock informations
  if (explo->is_critical_transition_explorer())
    verbose = false;

  s_mc_message_int_t request;
  request.type  = MessageType::DEADLOCK_CHECK;
  request.value = verbose;
  xbt_assert(checker_side_->get_channel().send(request) == 0, "Could not check deadlock state");

  s_mc_message_int_t answer;
  ssize_t received = checker_side_->get_channel().receive(answer);
  xbt_assert(received != -1, "Could not receive message");
  xbt_assert(received == sizeof answer, "Broken message (size=%zd; expected %zu)", received, sizeof answer);
  xbt_assert(answer.type == MessageType::DEADLOCK_CHECK_REPLY,
             "Received unexpected message %s (%i); expected MessageType::DEADLOCK_CHECK_REPLY (%i)",
             to_c_str(answer.type), (int)answer.type, (int)MessageType::DEADLOCK_CHECK_REPLY);

  if (answer.value != 0) {
    if (verbose) {
      XBT_CINFO(mc_global, "Counter-example execution trace:");
      for (auto const& frame : explo->get_textual_trace())
        XBT_CINFO(mc_global, "  %s", frame.c_str());
      XBT_INFO("You can debug the problem (and see the whole details) by rerunning out of simgrid-mc with "
               "--cfg=model-check/replay:'%s'",
               explo->get_record_trace().to_string().c_str());
      explo->log_state();
    }
    return true;
  }
  return false;
}

void RemoteApp::wait_for_requests()
{
  checker_side_->wait_for_requests();
}

Transition* RemoteApp::handle_simcall(aid_t aid, int times_considered, bool new_transition)
{
  XBT_DEBUG("Handle simcall of pid %d (time considered: %d; new? %s)", (int)aid, times_considered,
            (new_transition ? "yes" : "no"));

  return checker_side_->handle_simcall(aid, times_considered, new_transition);
}

void RemoteApp::replay_sequence(std::deque<std::pair<aid_t, int>> to_replay)
{

  checker_side_->handle_replay(to_replay);
}

void RemoteApp::finalize_app(bool terminate_asap)
{
  XBT_DEBUG("Finalise the application");
  checker_side_->finalize(terminate_asap);
}

} // namespace simgrid::mc
