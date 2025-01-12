/* Copyright (c) 2010-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

/* This example shows how to simulate a non-linear resource sharing for
 * network links.
 */

#include <simgrid/s4u.hpp>
#include <string>

namespace sg4 = simgrid::s4u;

XBT_LOG_NEW_DEFAULT_CATEGORY(s4u_network_nonlinear, "Messages specific for this s4u example");

/*************************************************************************************************/
class Sender {
  int messages_count; /* - number of messages */
  int msg_size = 1e6; /* - message size in bytes */

public:
  explicit Sender(int count) : messages_count(count) {}
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
      auto* payload           = new std::string(msg_content);
      unsigned long long size = msg_size * (i + 1);
      XBT_INFO("Send '%s' to '%s, msg size: %llu'", msg_content.c_str(), mbox->get_cname(), size);

      /* Create a communication representing the ongoing communication, and store it in pending_comms */
      sg4::CommPtr comm = mbox->put_async(payload, size);
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

/*************************************************************************************************/
/** @brief Non-linear resource sharing for links
 *
 * Note that the callback is called twice in this example:
 * 1) link UP: with the number of active flows (from 9 to 1)
 * 2) link DOWN: with 0 active flows. A crosstraffic communication is happing
 * in the down link, but it's not considered as an active flow.
 */
static double link_nonlinear(const sg4::Link* link, double capacity, int n)
{
  /* emulates a degradation in link according to the number of flows
   * you probably want something more complex than that and based on real
   * experiments */
  capacity = std::min(capacity, capacity * (1.0 - (n - 1) / 10.0));
  XBT_INFO("Link %s, %d active communications, new capacity %lf", link->get_cname(), n, capacity);
  return capacity;
}

/** @brief Create a simple 2-hosts platform */
static void load_platform()
{
  /* Creates the platform
   *  ________                 __________
   * | Sender |===============| Receiver |
   * |________|    Link1      |__________|
   */
  auto* zone     = sg4::create_full_zone("Zone1");
  auto* sender   = zone->create_host("sender", 1)->seal();
  auto* receiver = zone->create_host("receiver", 1)->seal();

  auto* link = zone->create_split_duplex_link("link1", 1e6);
  /* setting same callbacks (could be different) for link UP/DOWN in split-duplex link */
  link->get_link_up()->set_sharing_policy(
      sg4::Link::SharingPolicy::NONLINEAR,
      std::bind(&link_nonlinear, link->get_link_up(), std::placeholders::_1, std::placeholders::_2));
  link->get_link_down()->set_sharing_policy(
      sg4::Link::SharingPolicy::NONLINEAR,
      std::bind(&link_nonlinear, link->get_link_down(), std::placeholders::_1, std::placeholders::_2));
  link->set_latency(10e-6)->seal();

  /* create routes between nodes */
  zone->add_route(sender, receiver, {link});
  zone->seal();

  /* create actors Sender/Receiver */
  sg4::Actor::create("receiver", receiver, Receiver(9));
  sg4::Actor::create("sender", sender, Sender(9));
}

/*************************************************************************************************/
int main(int argc, char* argv[])
{
  sg4::Engine e(&argc, argv);

  /* create platform */
  load_platform();

  /* runs the simulation */
  e.run();

  return 0;
}
