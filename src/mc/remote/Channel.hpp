/* Copyright (c) 2015-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef SIMGRID_MC_CHANNEL_HPP
#define SIMGRID_MC_CHANNEL_HPP

#include "src/mc/remote/mc_protocol.h"

#include <type_traits>

namespace simgrid::mc {

/** A channel for exchanging messages between model-checker and model-checked app
 *
 *  This abstracts away the way the messages are transferred. Currently, they
 *  are sent over a (connected) `SOCK_SEQPACKET` socket.
 */
class Channel {
  int socket_ = -1;
  template <class M> static constexpr bool messageType() { return std::is_class_v<M> && std::is_trivial_v<M>; }
  char buffer_[MC_MESSAGE_LENGTH];
  size_t buffer_size_ = 0;

public:
  Channel() = default;
  explicit Channel(int sock) : socket_(sock) {}
  Channel(int sock, Channel const& other);
  ~Channel();

  // No copy:
  Channel(Channel const&) = delete;
  Channel& operator=(Channel const&) = delete;

  // Send
  int send(const void* message, size_t size) const;
  int send(MessageType type) const
  {
    s_mc_message_t message = {type};
    return this->send(&message, sizeof(message));
  }
  /** @brief Send a message; returns 0 on success or errno on failure */
  template <class M> typename std::enable_if_t<messageType<M>(), int> send(M const& m) const
  {
    return this->send(&m, sizeof(M));
  }

  // Receive
  ssize_t receive(void* message, size_t size);
  template <class M> typename std::enable_if_t<messageType<M>(), ssize_t> receive(M& m)
  {
    return this->receive(&m, sizeof(M));
  }
  // Peek (receive without consuming)
  std::pair<bool, void*> peek(size_t size);

  // Write the type of the next message in the reference parameter, and return false if no message is to be read (socket
  // closed by peer)
  std::pair<bool, MessageType> peek_message_type();
  void reinject(const char* data, size_t size);
  bool has_pending_data() const { return buffer_size_ != 0; }

  // Socket handling
  int get_socket() const { return socket_; }
  void reset_socket(int socket) { socket_ = socket; }
};
} // namespace simgrid::mc

#endif
