/* Copyright (c) 2012-2014. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

package io.storage;
import org.simgrid.msg.Msg;
import org.simgrid.msg.Host;
import org.simgrid.msg.MsgException;

public class Main {
  private Main() {
    throw new IllegalAccessError("Utility class");
  }

  public static void main(String[] args) throws MsgException {
    Msg.init(args);
    if(args.length < 1) {
      Msg.info("Usage   : Storage platform_file ");
      Msg.info("example : Storage ../platforms/storage/storage.xml ");
      System.exit(1);
    }

    Msg.createEnvironment(args[0]);

    Host[] hosts = Host.all();
    new io.storage.Client(hosts[3],0).start();

    Msg.run();
    }
}
