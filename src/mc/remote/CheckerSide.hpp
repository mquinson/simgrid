/* Copyright (c) 2007-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_REMOTE_EVENTLOOP_HPP
#define SIMGRID_MC_REMOTE_EVENTLOOP_HPP

#include "src/mc/mc_forward.hpp"
#include "src/mc/remote/Channel.hpp"
#include "src/mc/transition/Transition.hpp"

#include <memory>

namespace simgrid::mc {

/* CheckerSide: All what the checker needs to interact with a given application process */

class CheckerSide {
  Channel channel_;
  pid_t pid_;
  static unsigned count_;
  // Because of the way we fork, the real app is our grandchild.
  // child_checker_ is a CheckerSide to our child that can waitpid our grandchild on our behalf
  CheckerSide* child_checker_ = nullptr;

  void handle_dead_child(int status); // Launched when the dying child is the PID we follow
  void handle_waitpid();              // Launched when receiving a sigchild

public:
  explicit CheckerSide(int socket, CheckerSide* child_checker);
  explicit CheckerSide(const std::vector<char*>& args);
  ~CheckerSide();

  // No copy:
  /* Create a new CheckerSide by forking the currently existing one, and connect it through the master_socket */
  std::unique_ptr<CheckerSide> clone(int master_socket, const std::string& master_socket_name);

  CheckerSide(CheckerSide const&) = delete;
  CheckerSide& operator=(CheckerSide const&) = delete;
  CheckerSide& operator=(CheckerSide&&) = delete;

  // To avoid file descriptor exhaustion
  static unsigned get_count() { return count_; }
  /* Communicating with the application */
  Channel const& get_channel() const { return channel_; }
  Channel& get_channel() { return channel_; }

  void sync_with_app();
  void wait_for_requests();

  /** Ask the application to run one step. A transition is built iff new_transition = true */
  Transition* handle_simcall(aid_t aid, int times_considered, bool new_transition);

  /** Ask the application to run a full sequence of transition. The checker is not receiving any thing before the
   *  end */
  void handle_replay(std::deque<std::pair<aid_t, int>> to_replay);

  /** Read the aid in the SIMCALL_EXECUTE message that is expected to be next on the wire */
  aid_t get_aid_of_next_transition();

  /** Ask the application to run post-mortem analysis, and maybe to stop ASAP */
  void finalize(bool terminate_asap = false);

  /* Interacting with the application process */
  pid_t get_pid() const { return pid_; }
};

} // namespace simgrid::mc

#endif
