/* Copyright (c) 2015-2024. The SimGrid Team.  All rights reserved.         */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/remote/Channel.hpp"
#include "src/mc/remote/mc_protocol.h"
#include "xbt/asserts.h"
#include "xbt/asserts.hpp"
#include <utility>
#include <xbt/log.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_channel, mc, "MC interprocess communication");

namespace simgrid::mc {
Channel::Channel(int sock, Channel const& other) : socket_(sock), buffer_size_(other.buffer_size_)
{
  XBT_DEBUG("Adopt %zu bytes buffered by father channel.", buffer_size_);
  if (buffer_size_ > 0)
    memcpy(buffer_, other.buffer_, buffer_size_);
}

Channel::~Channel()
{
  if (this->socket_ >= 0)
    close(this->socket_);
}

/** @brief Send a message; returns 0 on success or errno on failure */
int Channel::send(const void* message, size_t size) const
{
  if (size >= sizeof(int) && is_valid_MessageType(*static_cast<const int*>(message))) {
    XBT_DEBUG("Sending %s (%zu bytes sent)", to_c_str(*static_cast<const MessageType*>(message)), size);
  } else {
    XBT_DEBUG("Sending bytes directly (from address %p) (%zu bytes sent)", message, size);
    if (size == 0)
      XBT_WARN("Request to send a 0-sized message! Proceeding anyway.");
  }

  while (::send(this->socket_, message, size, 0) == -1) {
    if (errno != EINTR) {
      XBT_ERROR("Channel::send failure: %s", strerror(errno));
      return errno;
    }
  }
  return 0;
}

ssize_t Channel::receive(void* message, size_t size)
{
  ssize_t copied = 0;
  auto* whereto  = static_cast<char*>(message);
  size_t todo    = size;
  if (buffer_size_ > 0) {
    copied = std::min(size, buffer_size_);
    memcpy(message, buffer_, copied);
    memmove(buffer_, buffer_ + copied, buffer_size_ - copied);
    buffer_size_ -= copied;

    todo -= copied;
    whereto += copied;
  }
  ssize_t res = 0;
  if (todo > 0) {
    errno = 0;
    res   = recv(this->socket_, whereto, todo, 0);
    xbt_enforce(res != -1 || errno == EAGAIN, "Channel::receive failure: %s", strerror(errno));
    if (res == -1) {
      res = 0;
    }
  }
  XBT_DEBUG("%d Wanted %zu; Got %zd from buffer and %zd from network", getpid(), size, copied, res);
  res += copied;
  if (static_cast<size_t>(res) >= sizeof(int) && is_valid_MessageType(*static_cast<const int*>(message))) {
    XBT_DEBUG("%d Receive %s (requested %zu; received %zd at %p)", getpid(),
              to_c_str(*static_cast<const MessageType*>(message)), size, res, message);
  } else {
    XBT_DEBUG("Receive %zd bytes", res);
  }
  return res;
}
std::pair<bool, void*> Channel::peek(size_t size)
{
  while (buffer_size_ < size) {
    size_t todo = size - buffer_size_;
    int got     = recv(this->socket_, buffer_ + buffer_size_, todo, 0);
    xbt_assert(got != -1 || errno == EAGAIN, "Channel::receive failure: %s", strerror(errno));
    if (got == 0)
      return std::make_pair(false, nullptr);
    buffer_size_ += got;
  }
  XBT_DEBUG("%d Peek %s", getpid(), to_c_str(*((const MessageType*)buffer_)));

  return std::make_pair(true, buffer_);
}

std::pair<bool, MessageType> Channel::peek_message_type()
{
  if (buffer_size_ == 0) {
    int res = recv(this->socket_, buffer_, MC_MESSAGE_LENGTH, 0);
    xbt_assert(res >= 0, "Cannot receive any data while peeking the message type. Errno: %s (%d)", strerror(errno),
               errno);
    buffer_size_ += res;
    if (res == 0)
      return std::make_pair(false, MessageType::NONE);
  }

  return std::make_pair(true, ((s_mc_message_t*)buffer_)->type);
}

void Channel::reinject(const char* data, size_t size)
{
  xbt_assert(size > 0, "Cannot reinject less than one char (size: %lu)", size);
  xbt_assert(size + buffer_size_ < MC_MESSAGE_LENGTH,
             "Reinjecting these data would make the buffer to overflow. Please increase MC_MESSAGE_LENGTH in the code "
             "(or fix your code).");
  XBT_DEBUG("%d Reinject %zu bytes on top of %zu pre-existing bytes", getpid(), size, buffer_size_);
  memcpy(buffer_ + buffer_size_, data, size);
  buffer_size_ += size;
}
} // namespace simgrid::mc
