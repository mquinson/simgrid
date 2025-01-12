/* Copyright (c) 2010-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

/* This example shows how to serialize a set of communications going through a link using Link::set_concurrency_limit()
 *
 * This example is very similar to the other asynchronous communication examples, but messages get serialized by the platform.
 * Without this call to Link::set_concurrency_limit(2) in main, all messages would be received at the exact same timestamp since
 * they are initiated at the same instant and are of the same size. But with this extra configuration to the link, at most 2
 * messages can travel through the link at the same time.
 */

#include "simgrid/s4u.hpp"
#include <string>
namespace sg4 = simgrid::s4u;

XBT_LOG_NEW_DEFAULT_CATEGORY(s4u_async_serialize, "Messages specific for this s4u example");

class Sender {
  int messages_count; /* - number of messages */
  int msg_size;       /* - message size in bytes */

public:
  explicit Sender(int size, int count) : messages_count(count), msg_size(size) {}
  void operator()() const
  {
    // sphinx-doc: init-begin (this line helps the doc to build; ignore it)
    /* ActivitySet in which we store all ongoing communications */
    sg4::ActivitySet pending_comms;

    /* Mailbox to use */
    sg4::Mailbox* mbox = sg4::Mailbox::by_name("receiver");
    // sphinx-doc: init-end

    /* Start dispatching all messages to receiver */
    for (int i = 0; i < messages_count; i++) {
      std::string msg_content = "Message " + std::to_string(i);
      // Copy the data we send: the 'msg_content' variable is not a stable storage location.
      // It will be destroyed when this actor leaves the loop, ie before the receiver gets it
      auto* payload = new std::string(msg_content);

      XBT_INFO("Send '%s' to '%s'", msg_content.c_str(), mbox->get_cname());

      /* Create a communication representing the ongoing communication, and store it in pending_comms */
      sg4::CommPtr comm = mbox->put_async(payload, msg_size);
      pending_comms.push(comm);
    }

    XBT_INFO("Done dispatching all messages");

    /* Now that all message exchanges were initiated, wait for their completion in one single call */
    pending_comms.wait_all();
    // sphinx-doc: put-end

    XBT_INFO("Goodbye now!");
  }
};

/* Receiver actor expects 1 argument: number of messages to be received */
class Receiver {
  sg4::Mailbox* mbox;
  int messages_count = 10; /* - number of messages */

public:
  explicit Receiver(int count) : messages_count(count) { mbox = sg4::Mailbox::by_name("receiver"); }
  void operator()()
  {
    /* Where we store all incoming msgs */
    std::map<sg4::CommPtr, std::shared_ptr<std::string*>> pending_msgs;
    sg4::ActivitySet pending_comms;

    XBT_INFO("Wait for %d messages asynchronously", messages_count);
    for (int i = 0; i < messages_count; i++) {
      auto msg  = std::make_shared<std::string*>();
      auto comm = mbox->get_async<std::string>(msg.get());
      pending_comms.push(comm);
      pending_msgs.insert({comm, msg});
    }

    while (not pending_comms.empty()) {
      auto completed_one = pending_comms.wait_any();
      if (completed_one != nullptr){
        auto comm = boost::dynamic_pointer_cast<sg4::Comm>(completed_one);
        auto msg = *pending_msgs[comm];
        XBT_INFO("I got '%s'.", msg->c_str());
        /* cleanup memory and remove from map */
        delete msg;
        pending_msgs.erase(comm);
      }
    }
  }
};

int main(int argc, char* argv[])
{
  sg4::Engine e(&argc, argv);
  /* Creates the platform
   *  ________                 __________
   * | Sender |===============| Receiver |
   * |________|    Link1      |__________|
   */
  auto* zone     = sg4::create_full_zone("Zone1");
  auto* sender   = zone->create_host("sender", 1)->seal();
  auto* receiver = zone->create_host("receiver", 1)->seal();

  /* create split-duplex link1 (UP/DOWN), limiting the number of concurrent flows in it for 2 */
  const auto* link =
      zone->create_split_duplex_link("link1", 10e9)->set_latency(10e-6)->set_concurrency_limit(2)->seal();

  /* create routes between nodes */
  zone->add_route(sender, receiver, {link});
  zone->seal();

  /* create actors Sender/Receiver */
  sg4::Actor::create("receiver", receiver, Receiver(10));
  sg4::Actor::create("sender", sender, Sender(1e6, 10));

  e.run();

  return 0;
}
