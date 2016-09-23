/* Copyright (c) 2012-2014, 2016. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

package app.centralizedmutex;
import java.util.LinkedList;

import org.simgrid.msg.Msg;
import org.simgrid.msg.Host;
import org.simgrid.msg.Task;
import org.simgrid.msg.Process;
import org.simgrid.msg.MsgException;

public class Coordinator extends Process {
  LinkedList<RequestTask> waitingQueue=new LinkedList<>();
  int csToServe;

  public Coordinator(Host host, String name, String[]args) {
    super(host,name,args);
  }

  public void main(String[] args) throws MsgException {
    csToServe = Integer.parseInt(args[0]);
    Task task;
    while (csToServe >0) {
      task = Task.receive("coordinator");
      if (task instanceof RequestTask) {
        RequestTask t = (RequestTask) task;
        if (waitingQueue.isEmpty()) {
          Msg.info("Got a request from "+t.from+". Queue empty: grant it");
          GrantTask tosend =  new GrantTask();
          tosend.send(t.from);
        } else {
          waitingQueue.addFirst(t);
        }
      } else if (task instanceof ReleaseTask) {
        if (!waitingQueue.isEmpty()) {
          RequestTask req = waitingQueue.removeLast();
          GrantTask tosend = new GrantTask();
          tosend.send(req.from);
        }
        csToServe--;
        if (waitingQueue.isEmpty() && csToServe==0) {
          Msg.info("we should shutdown the simulation now");
        }
      }
    }
  }
}
