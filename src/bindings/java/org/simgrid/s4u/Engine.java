/* Copyright (c) 2024-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

package org.simgrid.s4u;

public class Engine {
  private transient long swigCPtr;
  protected transient boolean swigCMemOwn;

  protected Engine(long cPtr, boolean cMemoryOwn) {
    swigCMemOwn = cMemoryOwn;
    swigCPtr = cPtr;
  }

  protected static long getCPtr(Engine obj) {
    return (obj == null) ? 0 : obj.swigCPtr;
  }

  protected static long swigRelease(Engine obj) {
    long ptr = 0;
    if (obj != null) {
      if (!obj.swigCMemOwn)
        throw new RuntimeException("Cannot release ownership as memory is not owned");
      ptr = obj.swigCPtr;
      obj.swigCMemOwn = false;
      obj.delete();
    }
    return ptr;
  }

  @SuppressWarnings({"deprecation", "removal"})
  protected void finalize() {
    delete();
  }

  public synchronized void delete() {
    if (swigCPtr != 0) {
      if (swigCMemOwn) {
        swigCMemOwn = false;
        simgridJNI.delete_Engine(swigCPtr);
      }
      swigCPtr = 0;
    }
  }

  public Engine(String[] args) { this(simgridJNI.new_Engine(args), true); }
  public String[] get_args() { return simgridJNI.Engine_get_args(swigCPtr); }

  public void run() {
    simgridJNI.Engine_run(swigCPtr, this);
  }

  public void run_until(double max_date) {
    simgridJNI.Engine_run_until(swigCPtr, this, max_date);
  }

  public static double get_clock() {
    return simgridJNI.Engine_get_clock();
  }

  public static Engine get_instance() {
    long cPtr = simgridJNI.Engine_get_instance();
    return (cPtr == 0) ? null : new Engine(cPtr, false);
  }

  public void load_platform(String platf) {
    simgridJNI.Engine_load_platform(swigCPtr, this, platf);
  }

  public void seal_platform() {
    simgridJNI.Engine_seal_platform(swigCPtr, this);
  }

  public String flatify_platform() {
    return simgridJNI.Engine_flatify_platform(swigCPtr, this);
  }

  public void load_deployment(String deploy) { simgridJNI.Engine_load_deployment(swigCPtr, deploy); }

  public long get_host_count() {
    return simgridJNI.Engine_get_host_count(swigCPtr, this);
  }

  public Host[] get_all_hosts() { return simgridJNI.Engine_get_all_hosts(swigCPtr, this); }

  public Host[] get_hosts_from_MPI_hostfile(String hostfile)
  {
    return simgridJNI.Engine_get_hosts_from_MPI_hostfile(swigCPtr, this, hostfile);
  }

  /** If no host of that name exists, throws an IllegalArgumentError. */
  public Host host_by_name(String name) {
    long cPtr = simgridJNI.Engine_host_by_name(swigCPtr, this, name);
    return (cPtr == 0) ? null : new Host(cPtr);
  }

  public long get_link_count() {
    return simgridJNI.Engine_get_link_count(swigCPtr, this);
  }

  public void track_vetoed_activities() { simgridJNI.Engine_track_vetoed_activities(swigCPtr); }
  public Activity[] get_vetoed_activities() { return simgridJNI.Engine_get_vetoed_activities(swigCPtr); }
  public void clear_vetoed_activities() { simgridJNI.Engine_clear_vetoed_activities(swigCPtr); }

  public Link[] get_all_links() { return simgridJNI.Engine_get_all_links(swigCPtr, this); }

  /** If no link of that name exists, throws an IllegalArgumentError. */
  public Link link_by_name(String name) {
    long cPtr = simgridJNI.Engine_link_by_name(swigCPtr, this, name);
    return (cPtr == 0) ? null : new Link(cPtr, false);
  }

  /** If no mailbox of that name exists, create it. */
  public Mailbox mailbox_by_name(String name)
  {
    long cPtr = simgridJNI.Engine_mailbox_by_name_or_create(swigCPtr, this, name);
    return (cPtr == 0) ? null : new Mailbox(cPtr);
  }

  /** If no message queue of that name exists, create it. */
  public MessageQueue message_queue_by_name(String name)
  {
    long cPtr = simgridJNI.Engine_message_queue_by_name_or_create(swigCPtr, this, name);
    return (cPtr == 0) ? null : new MessageQueue(cPtr, false);
  }

  public Actor add_actor(String name, Host location, Actor actor)
  {
    simgridJNI.Actor_create(name, Host.getCPtr(location), location, actor);
    return actor;
  }
  public long get_actor_count() {
    return simgridJNI.Engine_get_actor_count(swigCPtr, this);
  }

  public int get_actor_max_pid() {
    return simgridJNI.Engine_get_actor_max_pid(swigCPtr, this);
  }

  public Actor[] get_all_actors() { return simgridJNI.Engine_get_all_actors(swigCPtr, this); }

  public NetZone get_netzone_root()
  {
    long cPtr = simgridJNI.Engine_get_netzone_root(swigCPtr, this);
    return (cPtr == 0) ? null : new NetZone(cPtr);
  }

  public NetZone[] get_all_netzones() { return simgridJNI.Engine_get_all_netzones(swigCPtr, this); }

  /** If no link of that name exists, throws an IllegalArgumentError. */
  public NetZone netzone_by_name(String name)
  {
    long cPtr = simgridJNI.Engine_netzone_by_name_or_null(swigCPtr, this, name);
    if (cPtr == 0)
      throw new IllegalArgumentException("NetZone '" + name + "' does not exist. Did you spell its name correctly?");

    return (cPtr == 0) ? null : new NetZone(cPtr);
  }

  public static void set_config(String str) {
    simgridJNI.Engine_set_config__SWIG_0(str);
  }

  public static void set_config(String name, int value) {
    simgridJNI.Engine_set_config__SWIG_1(name, value);
  }

  public static void set_config(String name, boolean value) {
    simgridJNI.Engine_set_config__SWIG_2(name, value);
  }

  public static void set_config(String name, double value) {
    simgridJNI.Engine_set_config__SWIG_3(name, value);
  }

  public static void set_config(String name, String value) {
    simgridJNI.Engine_set_config__SWIG_4(name, value);
  }

  public void plugin_vm_live_migration_init() { simgridJNI.Engine_plugin_vm_live_migration_init(swigCPtr); }
  public void plugin_host_energy_init() { simgridJNI.Engine_plugin_host_energy_init(swigCPtr); }
  public void plugin_link_energy_init() { simgridJNI.Engine_plugin_link_energy_init(swigCPtr); }
  public void plugin_wifi_energy_init() { simgridJNI.Engine_plugin_wifi_energy_init(swigCPtr); }

  public static void on_platform_created_cb(CallbackVoid cb) { simgridJNI.Engine_on_platform_created_cb(cb); }

  public static void on_platform_creation_cb(CallbackVoid cb) { simgridJNI.Engine_on_platform_creation_cb(cb); }

  public static void on_simulation_start_cb(CallbackVoid cb) { simgridJNI.Engine_on_simulation_start_cb(cb); }

  public static void on_simulation_end_cb(CallbackVoid cb) { simgridJNI.Engine_on_simulation_end_cb(cb); }

  public static void on_time_advance_cb(CallbackDouble cb) { simgridJNI.Engine_on_time_advance_cb(cb); }

  public static void on_deadlock_cb(CallbackVoid cb) { simgridJNI.Engine_on_deadlock_cb(cb); }

  public static void die(String fmt, Object... args) { simgridJNI.Engine_die(String.format(fmt, args)); }

  public static void critical(String fmt, Object... args) { simgridJNI.Engine_critical(String.format(fmt, args)); }

  public static void error(String fmt, Object... args) { simgridJNI.Engine_error(String.format(fmt, args)); }

  public static void warn(String fmt, Object... args) { simgridJNI.Engine_warn(String.format(fmt, args)); }

  public static void info(String fmt, Object... args) { simgridJNI.Engine_info(String.format(fmt, args)); }

  public static void verbose(String fmt, Object... args) { simgridJNI.Engine_verbose(String.format(fmt, args)); }

  public static void debug(String fmt, Object... args) { simgridJNI.Engine_debug(String.format(fmt, args)); }

  public Activity[] create_DAG_from_dot(String filename) { return simgridJNI.create_DAG_from_dot(filename); }

  public Activity[] create_DAG_from_DAX(String filename) { return simgridJNI.create_DAG_from_DAX(filename); }

  public Activity[] create_DAG_from_json(String filename) { return simgridJNI.create_DAG_from_json(filename); }

  /** Example launcher. You can use it or provide your own launcher, as you wish */
  public static void main(String[] args)
  {

    /* initialize the SimGrid simulation. Must be done before anything else */
    Engine e = new Engine(args);

    if (args.length < 2) {
      Engine.info("Usage: org.simgrid.s4u.Engine platform_file deployment_file");
      System.exit(1);
    }

    /* Load the platform and deploy the application */
    e.load_platform(args[0]);
    e.load_deployment(args[1]);
    /* Execute the simulation */
    e.run();
  }

        /* Class initializer, to initialize various JNI stuff */
        static { org.simgrid.s4u.NativeLib.nativeInit(); }
}
