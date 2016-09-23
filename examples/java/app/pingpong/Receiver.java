/* Copyright (c) 2006-2014. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

package app.pingpong;
import org.simgrid.msg.Msg;
import org.simgrid.msg.Task;
import org.simgrid.msg.Process;
import org.simgrid.msg.MsgException;
import org.simgrid.msg.NativeException;
import org.simgrid.msg.HostNotFoundException;

public class Receiver extends Process {
  private static final double COMM_SIZE_BW = 100000000;
  public Receiver(String hostname, String name, String[]args) throws HostNotFoundException, NativeException{
    super(hostname,name,args);
  }

  public void main(String[] args) throws MsgException {
    Msg.info("hello!");

    Msg.info("try to get a task");

    PingPongTask task = (PingPongTask)Task.receive(getHost().getName());
    double timeGot = Msg.getClock();
    double timeSent = task.getTime();

    Msg.info("Got at time "+ timeGot);
    Msg.info("Was sent at time "+timeSent);
    double time = timeSent;

    double communicationTime = timeGot - time;
    Msg.info("Communication time : " + communicationTime);
    Msg.info(" --- bw "+ COMM_SIZE_BW/communicationTime + " ----");
    Msg.info("goodbye!");
  }
}