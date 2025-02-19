/* Copyright (c) 2024-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

package org.simgrid.s4u;

public abstract class Actor {
  protected Actor() {}
  /**
   * this is the method you should implement in your actor (it's the main of your actor).
   * @todo: make this abstract
   */
  public abstract void run() throws SimgridException; /* your code here */

  /* Internal method executing your code, and catching the actor-killing exception */
  public void do_run(long cPtr)
  {
    this.swigCPtr = cPtr;
    try {
      run();
    } catch (org.simgrid.s4u.ForcefulKillException e) {
      /* Actor killed, this is fine. */
    } catch (SimgridException e) {
      Engine.error("Actor killed by a %s exception: %s", e.getClass().getName(), e.getCause());
      e.printStackTrace();
    }
  }

  private transient long swigCPtr;

  protected static long getCPtr(Actor obj) {
    return (obj == null) ? 0 : obj.swigCPtr;
  }

  @SuppressWarnings({"deprecation", "removal"})
  protected void finalize() {
    delete();
  }

  public synchronized void delete() {
    if (swigCPtr != 0)
      simgridJNI.delete_Actor(swigCPtr);
    swigCPtr = 0;
  }

  public static Actor self() { return simgridJNI.Actor_self(); }
  public Engine get_engine() { return new Engine(simgridJNI.Engine_get_instance(), false); }

  public void on_this_suspend_cb(CallbackActor cb) { simgridJNI.Actor_on_this_suspend_cb(swigCPtr, this, cb); }
  public void on_this_resume_cb(CallbackActor cb) { simgridJNI.Actor_on_this_resume_cb(swigCPtr, this, cb); }
  public void on_this_sleep_cb(CallbackActor cb) { simgridJNI.Actor_on_this_sleep_cb(swigCPtr, this, cb); }
  public void on_this_wake_up_cb(CallbackActor cb) { simgridJNI.Actor_on_this_wake_up_cb(swigCPtr, this, cb); }

  public void on_this_host_change_cb(CallbackActorHost cb)
  {
    simgridJNI.Actor_on_this_host_change_cb(swigCPtr, this, cb);
  }

  public static void on_termination_cb(CallbackActor cb) { simgridJNI.Actor_on_termination_cb(cb); }
  public void on_this_termination_cb(CallbackActor cb) { simgridJNI.Actor_on_this_termination_cb(swigCPtr, this, cb); }

  public static void on_destruction_cb(CallbackActor cb) { simgridJNI.Actor_on_destruction_cb(cb); }
  public void on_this_destruction_cb(CallbackActor cb) { simgridJNI.Actor_on_this_destruction_cb(swigCPtr, this, cb); }

  public Actor daemonize() {
    simgridJNI.Actor_daemonize(swigCPtr, this);
    return this;
  }

  public boolean is_daemon() {
    return simgridJNI.Actor_is_daemon(swigCPtr, this);
  }

  public String get_name() {
    return simgridJNI.Actor_get_name(swigCPtr, this);
  }

  public Host get_host() {
    long cPtr = simgridJNI.Actor_get_host(swigCPtr, this);
    return (cPtr == 0) ? null : new Host(cPtr);
  }

  public int get_pid() {
    return simgridJNI.Actor_get_pid(swigCPtr, this);
  }

  public int get_ppid() {
    return simgridJNI.Actor_get_ppid(swigCPtr, this);
  }

  public void suspend() {
    simgridJNI.Actor_suspend(swigCPtr, this);
  }

  public void resume() {
    simgridJNI.Actor_resume(swigCPtr, this);
  }

  public boolean is_suspended() {
    return simgridJNI.Actor_is_suspended(swigCPtr, this);
  }

  public Actor set_auto_restart(boolean autorestart) {
    simgridJNI.Actor_set_auto_restart(swigCPtr, this, autorestart);
    return this;
  }

  public Actor set_auto_restart() {
    simgridJNI.Actor_set_auto_restart(swigCPtr, this, true);
    return this;
  }

  public int get_restart_count() {
    return simgridJNI.Actor_get_restart_count(swigCPtr, this);
  }

  public void set_kill_time(double time) {
    simgridJNI.Actor_set_kill_time(swigCPtr, this, time);
  }

  public double get_kill_time() {
    return simgridJNI.Actor_get_kill_time(swigCPtr, this);
  }

  public void set_host(Host new_host) {
    simgridJNI.Actor_set_host(swigCPtr, this, Host.getCPtr(new_host), new_host);
  }

  public void kill() {
    simgridJNI.Actor_kill(swigCPtr, this);
  }

  public static Actor by_pid(int pid) { return simgridJNI.Actor_by_pid(pid); }

  public void join() {
    simgridJNI.Actor_join__SWIG_0(swigCPtr, this);
  }

  public void join(double timeout) {
    simgridJNI.Actor_join__SWIG_1(swigCPtr, this, timeout);
  }

  public Actor restart() {
    swigCPtr = simgridJNI.Actor_restart(swigCPtr, this);
    return this;
  }

  public static void kill_all() {
    simgridJNI.Actor_kill_all();
  }

  public String get_property(String key) {
    return simgridJNI.Actor_get_property(swigCPtr, this, key);
  }

  public void set_property(String key, String value) {
    simgridJNI.Actor_set_property(swigCPtr, this, key, value);
  }
  /** Only call this on the current thread (on `this`) */
  protected void sleep_for(double duration) { simgridJNI.Actor_sleep_for(swigCPtr, this, duration); }
  /** Only call this on the current thread (on `this`) */
  public void sleep_until(double wakeup_time) { simgridJNI.Actor_sleep_until(swigCPtr, this, wakeup_time); }
  /** Only call this on the current thread (on `this`) */
  public void execute(double flop) { simgridJNI.Actor_execute__SWIG_0(swigCPtr, this, flop); }
  /** Only call this on the current thread (on `this`) */
  public void execute(double flop, double priority)
  {
    simgridJNI.Actor_execute__SWIG_1(swigCPtr, this, flop, priority);
  }
  /** Only call this on the current thread (on `this`) */
  public void thread_execute(Host host, double flop_amounts, int thread_count)
  {
    simgridJNI.Actor_thread_execute(swigCPtr, this, Host.getCPtr(host), host, flop_amounts, thread_count);
  }
  public void parallel_execute(Host[] hosts, double[] flops_amounts, double[] bytes_amounts)
  {
    long[] jhosts = new long[hosts.length];
    for (int i = 0; i < hosts.length; i++)
      jhosts[i] = Host.getCPtr(hosts[i]);
    simgridJNI.Actor_parallel_execute(swigCPtr, this, jhosts, flops_amounts, bytes_amounts);
  }
  /** Only call this on the current thread (on `this`) */
  public Exec exec_init(double flops_amounts)
  {
    return new Exec(simgridJNI.Actor_exec_seq_init(swigCPtr, this, flops_amounts), true);
  }
  public Exec exec_init(Host[] hosts, double[] flops_amounts, double[] bytes_amounts)
  {
    long[] jhosts = new long[hosts.length];
    for (int i = 0; i < hosts.length; i++)
      jhosts[i] = Host.getCPtr(hosts[i]);
    return new Exec(simgridJNI.Actor_exec_par_init(swigCPtr, this, jhosts, flops_amounts, bytes_amounts), true);
  }

  public Exec exec_async(double flops_amounts)
  {
    return new Exec(simgridJNI.Actor_exec_async(swigCPtr, this, flops_amounts), true);
  }

  public void yield() { simgridJNI.Actor_yield(swigCPtr, this); }

  public void exit() { simgridJNI.Actor_exit(swigCPtr, this); }

  public void on_exit(CallbackBoolean code) { simgridJNI.Actor_on_exit(swigCPtr, code); }
}
