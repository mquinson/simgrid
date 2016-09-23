/* Copyright (c) 2012-2014. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

package cloud.masterworker;

import java.util.ArrayList;

import org.simgrid.msg.Msg;
import org.simgrid.msg.VM;
import org.simgrid.msg.Host;
import org.simgrid.msg.Task;
import org.simgrid.msg.Process;
import org.simgrid.msg.MsgException;

public class Master extends Process {
  private Host[] hosts;

  public Master(Host host, String name, Host[] hosts) {
    super(host,name,null);
    this.hosts = hosts;
  }

  public void main(String[] args) throws MsgException {
    int workersCount = Main.NHOSTS;
    ArrayList<VM> vms = new ArrayList<>();

    // Create one VM per host and bind a process inside each one. 
    for (int i = 0; i < workersCount; i++) {
      Msg.info("create VM0"+i);  
      VM vm = new VM(hosts[i+1],"VM0"+i);
      vm.start();
      vms.add(vm);
      Worker worker= new Worker(vm,i);
      Msg.info("Put Worker "+worker.getName()+ " on "+vm.getName());
      worker.start();
    }

    Msg.info("Launched " + vms.size() + " VMs");

    Msg.info("Send a first batch of work to everyone");
    workBatch(workersCount);

    Msg.info("Suspend all VMs");
    for (int i = 0; i < vms.size(); i++) {
      Msg.info("Suspend "+vms.get(i).getName());
      vms.get(i).suspend();
    }

    Msg.info("Wait a while");
    waitFor(2);

    Msg.info("Resume all VMs.");
    for (int i = 0; i < vms.size(); i++) {
      vms.get(i).resume();
    }

    Msg.info("Sleep long enough for everyone to be done with previous batch of work");
    waitFor(1000 - Msg.getClock());

/*    Msg.info("Add one more process per VM.");
    for (int i = 0; i < vms.size(); i++) {
      VM vm = vms.get(i);
      Worker worker = new Worker(vm,i + vms.size());
      worker.start();
    }

    workBatch(workersCount * 2);
*/

    Msg.info("Migrate everyone to "+hosts[3].getName());
    for (int i = 0; i < vms.size(); i++) {
      Msg.info("Migrate "+vms.get(i).getName()+"from"+hosts[i+1].getName()+"to "+hosts[3].getName());
      vms.get(i).migrate(hosts[3]);
    }

    Msg.info("Let's shut down the simulation and kill everyone.");

    for (int i = 0; i < vms.size(); i++) {
      vms.get(i).destroy();
    }
    Msg.info("Master done.");
  }

  public void workBatch(int workersCount) throws MsgException {
    for (int i = 0; i < workersCount; i++) {
      Task task = new Task("Task0" + i, Main.TASK_COMP_SIZE, Main.TASK_COMM_SIZE);
      Msg.info("Sending to WRK0" + i);
      task.send("MBOX:WRK0" + i);
    }
  }
}
