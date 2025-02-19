/* simgrid-java.cpp - Native code of the Java bindings                      */

/* Copyright (c) 2024-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

// https://developer.ibm.com/articles/j-jni/ "Best practices for using the JNI"

/*
 * We pass raw pointers to the Java world, that builds empty shells around these pointers.
 * The refcounting still works, as we use intrusive_ptr for most of our objects (but the ones
 * that never get destroyed -- Host and Mailbox).
 */

/*
 * Every call to the C++ world that may block should try/catch the ForcefulKillException that
 * will be raised if the actor gets killed for whatever reason (including another actor
 * killing it). We may even need to protect every single call, not sure?
 * The return value (if any) is not important, as the Java code will deal with an exception
 * that cuts everything to stop that actor in the Java world too, not looking at this value.
 *
 * The idea is that when an actor needs to be forcefully killed, it was launched from:
 *   - (Java) Actor constructor: In most case, the ctor called manually in the user code
 *       but actors created from the deployment file use ctor(String[]), which calls the ctor().
 *   - (Java) once the actor is created, it's started by calling Engine::add_actor() which calls:
 *   - (C++) Actor_create(), does a C++->Java JNI call onto:
 *   - (Java) Actor.do_run(), which try/catch(ForcefullKillException) and call user's Actor.run()
 *
 * The decision to kill the actor is taken from C++ again (in another thread) at a point where
 * the victim is blocked in a Java->C++ call (any call is possible, even if blocking ones are
 * more probable). From C++, we set a Java exception in the Java killed actor and raise a C++
 * ForcefullKillException to rewind the C++ stack until the Java->C++ call on which the actor
 * is blocked. Which is why any such call must be wrapped in a try/catch block, to prevent the
 * exception from reaching the top-level exception handler.
 *
 * When reaching back the C++ call that is done on behalf of the Java actor run(), we swallow
 * that exception and give back the control to the Java code. Upon wakeup, the Java code raises
 * the requested Java exception, that is catch in Actor.do_run().
 *
 * This is how the actor jumps to the end of its execution in both C++/Java words.
 */

/*
 * The callbacks associated to signals are upcalls in the JNI parlance (calls from C++ to Java
 * functions, called on the C++ initiative). We don't use lambda in Java, but specific callback
 * classes, such as CallbackExec, which provide a virtual method called run(). There is one such
 * class per type of parameter taken by the run() method.
 *
 * In the C++ world, we cache the methodIds of these methods at initialization, and then, when
 * a callback is passed, we save the callback object in a lambda that is used as a C++ callback.
 *
 * For non-trivial objects (Exec, Comm, etc), a new java object is created for each C++ object
 * on which the lambda applies. This is inefficient, and we should cache the Java object as an
 * extension to the C++ objects. Still TBD and I'm afraid of refcounting issues if I add a ref.
 *
 * One extra difficulty is that we cannot pass the right JNIenv to the callback function, as we
 * don't really know which java thread will execute the signal (it's often maestro but not
 * always), so we have to retrieve that environment from the simgrid Context with get_env().
 * That's inefficient as retrieve that environment for every application of the C++ lambda, but
 * I don't see how this could be improved. C++ could use other signal signatures in the case of
 * Java executions?
 */

#include "simgrid/Exception.hpp"
#include "simgrid/plugins/energy.h"
#include "simgrid/plugins/live_migration.h"

#include "simgrid/s4u/Activity.hpp"
#include "simgrid/s4u/ActivitySet.hpp"
#include "simgrid/s4u/Actor.hpp"
#include "simgrid/s4u/Engine.hpp"
#include "simgrid/s4u/Host.hpp"
#include "src/kernel/context/Context.hpp"
#include "src/kernel/context/ContextJava.hpp"
#include "xbt/asserts.h"
#include "xbt/log.h"
#include "xbt/sysdep.h"

#include <boost/core/demangle.hpp>
#include <jni.h>
#include <stdexcept>
#include <stdlib.h>
#include <string.h>
#include <vector>

/* For the interactions with the other parts of simgrid */
JavaVM* simgrid_cached_jvm = nullptr;
JNIEnv* maestro_jenv       = nullptr;
extern bool do_install_signal_handlers;

/* For upcalls, from the C++ to the JVM */
static jmethodID CallbackActor_methodId     = 0;
static jmethodID CallbackActorHost_methodId = 0;
static jmethodID CallbackBoolean_methodId   = 0;
static jmethodID CallbackComm_methodId      = 0;
static jmethodID CallbackDisk_methodId      = 0;
static jmethodID CallbackDouble_methodId    = 0;
static jmethodID CallbackDHostDouble_methodId = 0;
static jmethodID CallbackExec_methodId      = 0;
static jmethodID CallbackIo_methodId        = 0;
static jmethodID CallbackLink_methodId      = 0;
static jmethodID CallbackNetzone_methodId   = 0;
static jmethodID CallbackVoid_methodId      = 0;
static jmethodID Actor_methodId             = 0;

/* Various cached classIds */
static jclass string_class = 0;
static jclass actor_class  = 0;

/* Various extensions */
struct ActorJavaExt {
  jobject jactor_;
  explicit ActorJavaExt(jobject jactor) : jactor_(jactor) {}
  static simgrid::xbt::Extension<simgrid::s4u::Actor, ActorJavaExt> EXTENSION_ID;
};
simgrid::xbt::Extension<simgrid::s4u::Actor, ActorJavaExt> ActorJavaExt::EXTENSION_ID;

/* This is not systematically used, only when storing an activity in an ActivitySet */
struct ActivityJavaExt {
  jobject jactivity_;
  explicit ActivityJavaExt(jobject jactivity) : jactivity_(jactivity) {}
  static simgrid::xbt::Extension<simgrid::s4u::Activity, ActivityJavaExt> EXTENSION_ID;
};
simgrid::xbt::Extension<simgrid::s4u::Activity, ActivityJavaExt> ActivityJavaExt::EXTENSION_ID;

static void exception_check_after_upcall(JNIEnv* jenv)
{
  jthrowable e = jenv->ExceptionOccurred();
  if (e) {
    jenv->ExceptionDescribe();
    jenv->ExceptionClear();
    throw std::runtime_error("Java exception occured");
  }
}
static void handle_exception(JNIEnv* jenv)
{
  if (jenv->ExceptionCheck()) {
    jenv->ExceptionDescribe();
    jenv->ExceptionClear();
  }
}
static JNIEnv* get_jenv()
{
  auto self = (simgrid::kernel::context::JavaContext*)simgrid::kernel::context::Context::self();
  if (self == nullptr || self->jenv_ == nullptr)
    return maestro_jenv;
  return self->jenv_;
}
static jmethodID init_methodId(JNIEnv* jenv, const char* klassname, const char* methname, const char* signature)
{
  char buff[1024];
  snprintf(buff, 1023, "org/simgrid/s4u/%s", klassname);
  jclass klass = jenv->FindClass(buff);
  handle_exception(jenv);
  xbt_assert(klass, "Cannot find class %s", klassname);

  jmethodID methodId = jenv->GetMethodID(klass, methname, signature); // void run(long e)
  handle_exception(jenv);
  xbt_assert(methodId, "Cannot find the method %s in the class %s", methname, klassname);
  return methodId;
}

static void get_classctor(JNIEnv* jenv, const char* klassname, const char* ctorsig, jclass& klass, jmethodID& ctor)
{
  klass = jenv->FindClass(klassname);
  xbt_assert(klass, "Java class %s not found", klassname);
  klass = (jclass)jenv->NewGlobalRef(klass);

  ctor = jenv->GetMethodID(klass, "<init>", ctorsig);
  xbt_assert(ctor, "Class %s does not seem to have a constructor of signature %s", klassname, ctorsig);
}

/* Retrive the jclass and ctor of the Java Disk object */
static std::pair<jclass, jmethodID> get_classctor_disk(JNIEnv* jenv)
{
  static jclass disk_class   = 0;
  static jmethodID disk_ctor = 0;
  if (disk_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/Disk", "(J)V", disk_class, disk_ctor);

  return std::make_pair(disk_class, disk_ctor);
}
static std::pair<jclass, jmethodID> get_classctor_host(JNIEnv* jenv)
{
  static jclass host_class   = 0;
  static jmethodID host_ctor = 0;
  if (host_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/Host", "(J)V", host_class, host_ctor);

  return std::make_pair(host_class, host_ctor);
}
static std::pair<jclass, jmethodID> get_classctor_link(JNIEnv* jenv)
{
  static jclass link_class   = 0;
  static jmethodID link_ctor = 0;
  if (link_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/Link", "(J)V", link_class, link_ctor);

  return std::make_pair(link_class, link_ctor);
}
static std::pair<jclass, jmethodID> get_classctor_netzone(JNIEnv* jenv)
{
  static jclass netzone_class   = 0;
  static jmethodID netzone_ctor = 0;
  if (netzone_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/NetZone", "(J)V", netzone_class, netzone_ctor);

  return std::make_pair(netzone_class, netzone_ctor);
}
static std::pair<jclass, jmethodID> get_classctor_activity(JNIEnv* jenv)
{
  static jclass da_class   = 0;
  static jmethodID da_ctor = 0;
  if (da_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/Activity", "(JZ)V", da_class, da_ctor);

  return std::make_pair(da_class, da_ctor);
}
static std::pair<jclass, jmethodID> get_classctor_comm(JNIEnv* jenv)
{
  static jclass da_class   = 0;
  static jmethodID da_ctor = 0;
  if (da_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/Comm", "(JZ)V", da_class, da_ctor);

  return std::make_pair(da_class, da_ctor);
}
static std::pair<jclass, jmethodID> get_classctor_exec(JNIEnv* jenv)
{
  static jclass da_class   = 0;
  static jmethodID da_ctor = 0;
  if (da_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/Exec", "(JZ)V", da_class, da_ctor);

  return std::make_pair(da_class, da_ctor);
}
static std::pair<jclass, jmethodID> get_classctor_io(JNIEnv* jenv)
{
  static jclass da_class   = 0;
  static jmethodID da_ctor = 0;
  if (da_class == 0)
    get_classctor(jenv, "org/simgrid/s4u/Io", "(JZ)V", da_class, da_ctor);

  return std::make_pair(da_class, da_ctor);
}

/* *********************************************************************************** */
/* Initialize the Java bindings                                                        */
/* *********************************************************************************** */
XBT_LOG_EXTERNAL_DEFAULT_CATEGORY(java);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* jvm, void* reserved)
{
  simgrid_cached_jvm = jvm;
  setlocale(LC_NUMERIC, "C");
  do_install_signal_handlers = false;
  return JNI_VERSION_1_2;
}

static struct SimGridJavaInit {
  SimGridJavaInit()
  {
    simgrid::kernel::context::ContextFactory::initializer = []() {
      XBT_INFO("Using regular java threads.");

      xbt_assert(simgrid_cached_jvm->AttachCurrentThread((void**)&maestro_jenv, NULL) == JNI_OK,
                 "The maestro thread could not be attached to the JVM");

      // Initialize the upcall mechanism
      CallbackActor_methodId = init_methodId(maestro_jenv, "CallbackActor", "run", "(Lorg/simgrid/s4u/Actor;)V");
      CallbackActorHost_methodId =
          init_methodId(maestro_jenv, "CallbackActorHost", "run", "(Lorg/simgrid/s4u/Actor;J)V");
      CallbackBoolean_methodId   = init_methodId(maestro_jenv, "CallbackBoolean", "run", "(Z)V");
      CallbackComm_methodId      = init_methodId(maestro_jenv, "CallbackComm", "run", "(J)V");
      CallbackDisk_methodId      = init_methodId(maestro_jenv, "CallbackDisk", "run", "(J)V");
      CallbackDouble_methodId    = init_methodId(maestro_jenv, "CallbackDouble", "run", "(D)V");
      CallbackDHostDouble_methodId = init_methodId(maestro_jenv, "CallbackDHostDouble", "run", "(JD)D");
      CallbackExec_methodId      = init_methodId(maestro_jenv, "CallbackExec", "run", "(J)V");
      CallbackIo_methodId        = init_methodId(maestro_jenv, "CallbackIo", "run", "(J)V");
      CallbackLink_methodId      = init_methodId(maestro_jenv, "CallbackLink", "run", "(J)V");
      CallbackNetzone_methodId   = init_methodId(maestro_jenv, "CallbackNetzone", "run", "(J)V");
      CallbackVoid_methodId      = init_methodId(maestro_jenv, "CallbackVoid", "run", "()V");

      Actor_methodId = init_methodId(maestro_jenv, "Actor", "do_run", "(J)V");

      string_class = (jclass)maestro_jenv->NewGlobalRef(maestro_jenv->FindClass("java/lang/String"));
      xbt_assert(string_class);
      actor_class = (jclass)maestro_jenv->NewGlobalRef(maestro_jenv->FindClass("org/simgrid/s4u/Actor"));
      xbt_assert(actor_class);

      // Initialize extensions
      ActorJavaExt::EXTENSION_ID = simgrid::s4u::Actor::extension_create<ActorJavaExt>();
      ActivityJavaExt::EXTENSION_ID = simgrid::s4u::Activity::extension_create<ActivityJavaExt>();

      // Initialize the factory mechanism
      return new simgrid::kernel::context::JavaContextFactory();
    };
  }
} sgJavaInit;

static void inline init_exception_class(JNIEnv* jenv, jclass& klass, const char* name)
{
  if (klass == 0) {
    klass = jenv->FindClass(name);
    xbt_assert(klass, "Class %s not found", name);
    klass = (jclass)jenv->NewGlobalRef(klass);
  }
}
static void rethrow_simgrid_exception(JNIEnv* jenv, std::exception const& e)
{
  static jclass assert_ex      = 0; // org/simgrid/s4u/AssertionError
  static jclass timeout_ex     = 0; // org/simgrid/s4u/TimeoutException
  static jclass networkfail_ex = 0; // org/simgrid/s4u/NetworkFailureException
  static jclass hostfail_ex    = 0; // org/simgrid/s4u/HostFailureException
  static jclass illegal_ex     = 0; // std::invalid_argument <-> java/lang/IllegalArgumentException

  jenv->ExceptionClear();

  if (dynamic_cast<const std::invalid_argument*>(&e)) {
    init_exception_class(jenv, illegal_ex, "java/lang/IllegalArgumentException");
    jenv->ThrowNew(illegal_ex, e.what());
  } else if (dynamic_cast<const simgrid::TimeoutException*>(&e) != nullptr) {
    init_exception_class(jenv, timeout_ex, "org/simgrid/s4u/TimeoutException");
    jenv->ThrowNew(timeout_ex, e.what());
  } else if (dynamic_cast<const simgrid::NetworkFailureException*>(&e) != nullptr) {
    init_exception_class(jenv, networkfail_ex, "org/simgrid/s4u/NetworkFailureException");
    jenv->ThrowNew(networkfail_ex, e.what());
  } else if (dynamic_cast<const simgrid::HostFailureException*>(&e) != nullptr) {
    init_exception_class(jenv, hostfail_ex, "org/simgrid/s4u/HostFailureException");
    jenv->ThrowNew(hostfail_ex, e.what());
  } else if (dynamic_cast<const simgrid::AssertionError*>(&e) != nullptr) {
    init_exception_class(jenv, assert_ex, "org/simgrid/s4u/AssertionError");
    jenv->ThrowNew(assert_ex, e.what());
  } else {
    XBT_INFO("Exception %s is not handled by the Java bindings", boost::core::demangle(typeid(e).name()).c_str());
    xbt_backtrace_display_current();
    THROW_UNIMPLEMENTED;
  }
}

/* Support for throwing Java exceptions */
typedef enum {
  SWIG_JavaOutOfMemoryError = 1,
  SWIG_JavaRuntimeException,
  SWIG_JavaNullPointerException,
} SWIG_JavaExceptionCodes;

typedef struct {
  SWIG_JavaExceptionCodes code;
  const char* java_exception;
} SWIG_JavaExceptions_t;

XBT_ATTRIB_UNUSED static void SWIG_JavaThrowException(JNIEnv* jenv, SWIG_JavaExceptionCodes code, const char* msg)
{
  jclass excep;
  static const SWIG_JavaExceptions_t java_exceptions[] = {
      {SWIG_JavaOutOfMemoryError, "java/lang/OutOfMemoryError"},
      {SWIG_JavaRuntimeException, "java/lang/RuntimeException"},
      {SWIG_JavaNullPointerException, "java/lang/NullPointerException"},
      {(SWIG_JavaExceptionCodes)0, "java/lang/UnknownError"}};
  const SWIG_JavaExceptions_t* except_ptr = java_exceptions;

  while (except_ptr->code != code && except_ptr->code)
    except_ptr++;

  jenv->ExceptionClear();
  excep = jenv->FindClass(except_ptr->java_exception);
  if (excep)
    jenv->ThrowNew(excep, msg);
}

static std::string java_string_to_std_string(JNIEnv* jenv, jstring jstr)
{
  if (!jstr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "null string");
    return std::string();
  }
  const char* pstr = (const char*)jenv->GetStringUTFChars(jstr, 0);
  if (!pstr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "null string");
    return std::string();
  }
  std::string str(pstr);
  jenv->ReleaseStringUTFChars(jstr, pstr);
  return str;
}
static std::vector<double> java_doublearray_to_vector(JNIEnv* jenv, jdoubleArray jarray)
{
  std::vector<double> res;
  int len          = jenv->GetArrayLength(jarray);
  double* cjvalues = jenv->GetDoubleArrayElements(jarray, nullptr);
  for (int i = 0; i < len; i++)
    res.push_back(cjvalues[i]);
  jenv->ReleaseDoubleArrayElements(jarray, cjvalues, JNI_ABORT);
  return res;
}
static std::vector<std::string> java_stringarray_to_vector(JNIEnv* jenv, jobjectArray jarray)
{
  std::vector<std::string> res;
  int len = jenv->GetArrayLength(jarray);
  for (int i = 0; i < len; i++)
    res.push_back(java_string_to_std_string(jenv, (jstring)jenv->GetObjectArrayElement(jarray, i)));
  return res;
}
#define check_javaexception(jenv)                                                                                      \
  do {                                                                                                                 \
    if (jenv->ExceptionCheck()) {                                                                                      \
      jenv->ExceptionDescribe();                                                                                       \
      jenv->ExceptionClear();                                                                                          \
    }                                                                                                                  \
  } while (0)

#define check_nullparam(val, msg)                                                                                      \
  do {                                                                                                                 \
    if (val == nullptr) {                                                                                              \
      SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, msg);                                               \
      return 0;                                                                                                        \
    }                                                                                                                  \
  } while (0)

#if defined(SWIG_JAVA_USE_THREAD_NAME)

#if !defined(SWIG_JAVA_GET_THREAD_NAME)
namespace Swig {
static int GetThreadName(char* name, size_t len);
}

#if defined(__linux__)

#include <sys/prctl.h>
static int Swig::GetThreadName(char* name, size_t len)
{
  (void)len;
#if defined(PR_GET_NAME)
  return prctl(PR_GET_NAME, (unsigned long)name, 0, 0, 0);
#else
  (void)name;
  return 1;
#endif
}

#elif defined(__unix__) || defined(__APPLE__)

#include <pthread.h>
static int Swig::GetThreadName(char* name, size_t len)
{
  return pthread_getname_np(pthread_self(), name, len);
}

#else

static int Swig::GetThreadName(char* name, size_t len)
{
  (void)len;
  (void)name;
  return 1;
}
#endif

#endif

#endif

#include <simgrid/s4u.hpp>
using namespace simgrid::s4u;
using namespace simgrid;

#ifdef __cplusplus
extern "C" {
#endif

/* *****************************************************************************************************************
 * The many JNI calls follow. The 4 first parameters are always the same:
 *  - JNIEnv* jenv: Access to the JVM in case you want some services (as always in JNI)
 *  - jclass jcls: not sure, it's not really used here but it's requested in JNI
 *  - jlong cthis: pointer to the C++ object embedded in an integer (as in SWIG)
 *  - jobject jthis: pointer to the Java (as in SWIG directors)
 *
 * Then, all classical parameters to the method follow, using stupid naming conventions inherited from swig.
 * *****************************************************************************************************************/

/* The following header file contains the prototypes of all JNI calls, generated with `javac -h` by cmake
   and located somewhere under simgrid_jar.dir/native_headers/org_simgrid_s4u_simgridJNI.h */
#include "org_simgrid_s4u_simgridJNI.h"

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1sleep_1for(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis, jdouble duration)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call sleep_for() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  try {
    simgrid::s4u::this_actor::sleep_for(duration);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1sleep_1until(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis, jdouble wakeup_time)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call sleep_until() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  try {
    simgrid::s4u::this_actor::sleep_until(wakeup_time);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1execute_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jdouble flop)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call execute() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  try {
    simgrid::s4u::this_actor::execute(flop);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1execute_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jdouble flop, jdouble priority)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call execute() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  try {
    simgrid::s4u::this_actor::execute(flop, priority);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1thread_1execute(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jlong chost,
                                                                               jobject jhost, jdouble flop_amounts,
                                                                               jint thread_count)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call thread_execute() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  try {
    simgrid::s4u::this_actor::thread_execute((Host*)chost, flop_amounts, thread_count);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}
JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1parallel_1execute(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jlongArray jhosts,
                                                                                jdoubleArray jcompute_amounts,
                                                                                jdoubleArray jcomm_amounts)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call parallel_execute() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");

  std::vector<Host*> chosts;
  if (jhosts) {
    int len        = jenv->GetArrayLength(jhosts);
    jlong* cjhosts = jenv->GetLongArrayElements(jhosts, nullptr);
    for (int i = 0; i < len; i++)
      chosts.push_back((Host*)cjhosts[i]);
    jenv->ReleaseLongArrayElements(jhosts, cjhosts, JNI_ABORT);
  }

  std::vector<double> ccompute_amounts;
  if (jcompute_amounts) {
    int len                   = jenv->GetArrayLength(jcompute_amounts);
    double* cjcompute_amounts = jenv->GetDoubleArrayElements(jcompute_amounts, nullptr);
    for (int i = 0; i < len; i++)
      ccompute_amounts.push_back(cjcompute_amounts[i]);
    jenv->ReleaseDoubleArrayElements(jcompute_amounts, cjcompute_amounts, JNI_ABORT);
  }

  std::vector<double> ccomm_amounts;
  if (jcomm_amounts) {
    int len                = jenv->GetArrayLength(jcomm_amounts);
    double* cjcomm_amounts = jenv->GetDoubleArrayElements(jcomm_amounts, nullptr);
    for (int i = 0; i < len; i++)
      ccomm_amounts.push_back(cjcomm_amounts[i]);
    jenv->ReleaseDoubleArrayElements(jcomm_amounts, cjcomm_amounts, JNI_ABORT);
  }

  try {
    simgrid::s4u::this_actor::parallel_execute(chosts, ccompute_amounts, ccomm_amounts);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (Exception const& ex) {
    rethrow_simgrid_exception(jenv, ex);
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1exec_1seq_1init(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jdouble flops_amounts)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call exec_init() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  auto result = simgrid::s4u::this_actor::exec_init(flops_amounts);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1exec_1par_1init(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jlongArray jhosts,
                                                                                jdoubleArray jcompute_amounts,
                                                                                jdoubleArray jcomm_amounts)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call exec_init() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");

  std::vector<Host*> chosts;
  int len        = jenv->GetArrayLength(jhosts);
  jlong* cjhosts = jenv->GetLongArrayElements(jhosts, nullptr);
  for (int i = 0; i < len; i++)
    chosts.push_back((Host*)cjhosts[i]);
  jenv->ReleaseLongArrayElements(jhosts, cjhosts, JNI_ABORT);

  std::vector<double> ccompute_amounts;
  len                       = jenv->GetArrayLength(jcompute_amounts);
  double* cjcompute_amounts = jenv->GetDoubleArrayElements(jcompute_amounts, nullptr);
  for (int i = 0; i < len; i++)
    ccompute_amounts.push_back(cjcompute_amounts[i]);
  jenv->ReleaseDoubleArrayElements(jcompute_amounts, cjcompute_amounts, JNI_ABORT);

  std::vector<double> ccomm_amounts;
  len                    = jenv->GetArrayLength(jcomm_amounts);
  double* cjcomm_amounts = jenv->GetDoubleArrayElements(jcomm_amounts, nullptr);
  for (int i = 0; i < len; i++)
    ccomm_amounts.push_back(cjcomm_amounts[i]);
  jenv->ReleaseDoubleArrayElements(jcomm_amounts, cjcomm_amounts, JNI_ABORT);

  auto result = simgrid::s4u::this_actor::exec_init(chosts, ccompute_amounts, ccomm_amounts);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1exec_1async(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis, jdouble flops_amounts)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call exec_async() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  auto result = simgrid::s4u::this_actor::exec_async(flops_amounts);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1set_1host(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis, jlong chost, jobject jhost)
{
  ((Actor*)cthis)->set_host((Host*)chost);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1yield(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                     jobject jthis)
{
  xbt_assert((Actor*)cthis == simgrid::s4u::Actor::self(),
             "You cannot call yield() on a remote actor %ld:%s, only on the currently executing actor.",
             cthis ? ((Actor*)cthis)->get_pid() : -1, cthis ? ((Actor*)cthis)->get_cname() : "null pointer");
  try {
    simgrid::s4u::this_actor::yield();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1exit(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                    jobject jthis)
{
  auto self = (Actor*)cthis;
  try {
    if (Actor::self() == self) {
      // Calling self->kill() makes the JVM to segfault, so let's kill the java actor in Java instead
      static jclass klass = 0;
      if (klass == 0) {
        klass = jenv->FindClass("org/simgrid/s4u/ForcefulKillException");
        xbt_assert(klass, "Class ForcefulKillException not found");
      }

      jenv->ThrowNew(klass, "Actor committed a suicide :( It should have talked to someone instead.");
    } else
      self->kill();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1termination_1cb(JNIEnv* jenv, jclass jcls,
                                                                                   jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Actor::on_termination_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1destruction_1cb(JNIEnv* jenv, jclass jcls,
                                                                                   jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Actor::on_destruction_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1exit(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject cb)
{
  if (cb) {
    try {
      cb = jenv->NewGlobalRef(cb);
      simgrid::s4u::this_actor::on_exit([cb](bool b) {
        get_jenv()->CallVoidMethod(cb, CallbackBoolean_methodId, b);
        exception_check_after_upcall(get_jenv());
      });
    } catch (ForcefulKillException const&) {
    }
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1self(JNIEnv* jenv, jclass jcls)
{
  Actor* self    = simgrid::s4u::Actor::self();
  jobject result = self ? self->extension<ActorJavaExt>()->jactor_ : nullptr;
  return result;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1this_1suspend_1cb(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Actor*)cthis)->on_this_suspend_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1this_1resume_1cb(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis,
                                                                                    jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Actor*)cthis)->on_this_resume_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1this_1sleep_1cb(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Actor*)cthis)->on_this_sleep_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1this_1wake_1up_1cb(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis,
                                                                                      jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Actor*)cthis)->on_this_wake_up_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1this_1host_1change_1cb(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Actor*)cthis)->on_this_host_change_cb([cb](Actor const& a, Host const& h) {
      get_jenv()->CallVoidMethod(cb, CallbackActorHost_methodId, a.extension<ActorJavaExt>()->jactor_, &h);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1this_1termination_1cb(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis, jobject jthis,
                                                                                         jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Actor*)cthis)->on_this_termination_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1on_1this_1destruction_1cb(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis, jobject jthis,
                                                                                         jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Actor*)cthis)->on_this_destruction_cb([cb](Actor const& a) {
      get_jenv()->CallVoidMethod(cb, CallbackActor_methodId, a.extension<ActorJavaExt>()->jactor_);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1daemonize(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  try {
    ((Actor*)cthis)->daemonize();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1is_1daemon(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  bool res = false;
  try {
    res = ((Actor*)cthis)->is_daemon();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return res;
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis)
{
  const char* result = ((Actor*)cthis)->get_cname();
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1get_1host(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis)
{
  return (jlong)((Actor*)cthis)->get_host();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1get_1pid(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis)
{
  return ((Actor*)cthis)->get_pid();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1get_1ppid(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  return ((Actor*)cthis)->get_ppid();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1suspend(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                       jobject jthis)
{
  try {
    ((Actor*)cthis)->suspend();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1resume(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                      jobject jthis)
{
  ((Actor*)cthis)->resume();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1is_1suspended(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  return ((Actor*)cthis)->is_suspended();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1set_1auto_1restart(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jboolean jarg2)
{
  ((Actor*)cthis)->set_auto_restart(jarg2);
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1get_1restart_1count(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  return ((Actor*)cthis)->get_restart_count();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1set_1kill_1time(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jdouble jarg2)
{
  ((Actor*)cthis)->set_kill_time(jarg2);
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1get_1kill_1time(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis)
{
  return ((Actor*)cthis)->get_kill_time();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1kill(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                    jobject jthis)
{
  try {
    ((Actor*)cthis)->kill();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1by_1pid(JNIEnv* jenv, jclass jcls, jint pid)
{
  ActorPtr result = simgrid::s4u::Actor::by_pid(pid);
  return result.get() != nullptr ? result->extension<ActorJavaExt>()->jactor_ : nullptr;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1join_1_1SWIG_10(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  try {
    ((Actor*)cthis)->join();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1join_1_1SWIG_11(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jdouble jarg2)
{
  try {
    ((Actor*)cthis)->join(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1restart(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis)
{
  ActorPtr result = ((Actor*)cthis)->restart();
  result->extension_set<ActorJavaExt>(new ActorJavaExt(jthis));
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1kill_1all(JNIEnv* jenv, jclass jcls)
{
  try {
    simgrid::s4u::Actor::kill_all();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1get_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jstring jprop)
{
  std::string cprop  = java_string_to_std_string(jenv, jprop);
  const char* result = ((Actor*)cthis)->get_property(cprop);
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1set_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jstring jprop, jstring jval)
{
  if (jprop != nullptr && jval != nullptr) {
    std::string cprop = java_string_to_std_string(jenv, jprop);
    std::string cval  = java_string_to_std_string(jenv, jval);
    ((Actor*)cthis)->set_property(cprop, cval);
  } else if (jprop == nullptr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property name shall not be null.");
  } else {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property value shall not be null.");
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Actor_1create(JNIEnv* jenv, jclass jcls, jstring jname,
                                                                       jlong chost, jobject jhost, jobject jactor)
{
  std::string name = java_string_to_std_string(jenv, jname);
  Host* host       = (Host*)chost;

  jactor          = jenv->NewGlobalRef(jactor);
  ActorPtr result = Actor::init(name, host);
  result->extension_set<ActorJavaExt>(new ActorJavaExt(jactor));
  result->start([jactor]() {
    auto jenv = ((simgrid::kernel::context::JavaContext*)simgrid::kernel::context::Context::self())->jenv_;

    jenv->CallVoidMethod(jactor, Actor_methodId, (jlong)simgrid::s4u::Actor::self());
    exception_check_after_upcall(get_jenv());
  });
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1Actor(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  intrusive_ptr_release((Actor*)cthis);
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1to_1c_1str(JNIEnv* jenv, jclass jcls, jint cthis)
{
  auto arg1          = (simgrid::s4u::Activity::State)cthis;
  const char* result = Activity::to_c_str(arg1);
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1is_1assigned(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->is_assigned();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1dependencies_1solved(JNIEnv* jenv, jclass jcls,
                                                                                           jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->dependencies_solved();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1has_1no_1successor(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->has_no_successor();
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1dependencies(JNIEnv* jenv, jclass jcls,
                                                                                            jlong cthis, jobject jthis)
{
  auto [activity_class, activity_ctor] = get_classctor_activity(jenv);
  auto [comm_class, comm_ctor]         = get_classctor_comm(jenv);
  auto [io_class, io_ctor]             = get_classctor_io(jenv);
  auto [exec_class, exec_ctor]         = get_classctor_exec(jenv);

  auto cdep         = ((Activity*)cthis)->get_dependencies();
  jobjectArray jres = jenv->NewObjectArray(cdep.size(), activity_class, nullptr);

  int i             = 0;
  for (ActivityPtr const& act : cdep) {
    intrusive_ptr_add_ref(act.get());
    if (dynamic_cast<Comm*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(comm_class, comm_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Io*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(io_class, io_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Exec*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(exec_class, exec_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else
      THROW_IMPOSSIBLE;
    i++;
  }

  return jres;
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1successors(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis)
{
  auto [activity_class, activity_ctor] = get_classctor_activity(jenv);
  auto [comm_class, comm_ctor]         = get_classctor_comm(jenv);
  auto [io_class, io_ctor]             = get_classctor_io(jenv);
  auto [exec_class, exec_ctor]         = get_classctor_exec(jenv);

  auto csucc        = ((Activity*)cthis)->get_successors();
  jobjectArray jres = jenv->NewObjectArray(csucc.size(), activity_class, nullptr);
  int i             = 0;
  for (ActivityPtr const& act : csucc) {
    intrusive_ptr_add_ref(act.get());
    if (dynamic_cast<Comm*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(comm_class, comm_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Io*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(io_class, io_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Exec*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(exec_class, exec_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else
      THROW_IMPOSSIBLE;
    i++;
  }

  return jres;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1Activity(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  intrusive_ptr_release((Activity*)cthis);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1start(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis)
{
  try {
    ((Activity*)cthis)->start();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1test(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  return ((Activity*)cthis)->test();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1await_1for(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jdouble jarg2)
{
  try {
    ((Activity*)cthis)->wait_for(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1await_1for_1or_1cancel(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis, jobject jthis,
                                                                                         jdouble jarg2)
{
  try {
    ((Activity*)cthis)->wait_for(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1await_1until(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jdouble jarg2)
{
  try {
    ((Activity*)cthis)->wait_until(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1cancel(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  ((Activity*)cthis)->cancel();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1state(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis)
{
  return (jint)((Activity*)cthis)->get_state();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1state_1str(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis)
{
  const char* result = ((Activity const*)cthis)->get_state_str();
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1is_1canceled(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->is_canceled();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1is_1failed(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  return ((Activity*)cthis)->is_failed();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1is_1done(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  return ((Activity*)cthis)->is_done();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1is_1detached(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->is_detached();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1suspend(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis)
{
  try {
    ((Activity*)cthis)->suspend();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1resume(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  try {
    ((Activity*)cthis)->resume();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1is_1suspended(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->is_suspended();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  const char* cname = ((Activity*)cthis)->get_cname();
  if (cname)
    return jenv->NewStringUTF(cname);
  return 0;
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1remaining(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->get_remaining();
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1start_1time(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->get_start_time();
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1finish_1time(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  return ((Activity*)cthis)->get_finish_time();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1mark(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                       jobject jthis)
{
  ((Activity*)cthis)->mark();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1is_1marked(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  return ((Activity*)cthis)->is_marked();
}
XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1get_1data(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  jobject cdata = ((Activity*)cthis)->get_data<_jobject>();
  if (cdata != nullptr)
    cdata = jenv->NewLocalRef(cdata);
  return cdata;
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Activity_1set_1data(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject obj)
{
  // Clean any object already stored
  jobject cdata = ((Activity*)cthis)->get_data<_jobject>();
  if (cdata != nullptr)
    jenv->DeleteGlobalRef(cdata);

  // Store the new object
  auto glob = obj;
  if (glob != nullptr)
    glob = jenv->NewGlobalRef(obj);

  ((Activity*)cthis)->set_data(glob);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_new_1ActivitySet(JNIEnv* jenv, jclass jcls)
{
  ActivitySetPtr result(new ActivitySet());
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1ActivitySet(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  intrusive_ptr_release(((ActivitySet*)cthis));
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1push(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis, jlong cactivity,
                                                                          jobject jactivity)
{
  try {
    ActivityPtr act((Activity*)cactivity);
    jobject glob = jenv->NewGlobalRef(jactivity);
    act->extension_set<ActivityJavaExt>(new ActivityJavaExt(glob));
    ((ActivitySet*)cthis)->push(act);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1erase(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis, jlong cactivity,
                                                                           jobject jactivity)
{
  try {
    ActivityPtr act((Activity*)cactivity);
    ((ActivitySet*)cthis)->erase(act);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}
XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1size(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis)
{
  try {
    return ((ActivitySet*)cthis)->size();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}
XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1empty(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  try {
    return ((ActivitySet*)cthis)->empty();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return false;
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1clear(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  try {
    ((ActivitySet*)cthis)->clear();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}
XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1at(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis, jint index)
{
  try {
    return jenv->NewLocalRef(((ActivitySet*)cthis)->at(index)->extension<ActivityJavaExt>()->jactivity_);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return nullptr;
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1await_1all_1for(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jdouble timeout)
{
  try {
    ((ActivitySet*)cthis)->wait_all_for(timeout);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1await_1all(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  try {
    ((ActivitySet*)cthis)->wait_all();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}
XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1test_1any(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis)
{
  try {
    ActivityPtr act = ((ActivitySet*)cthis)->test_any();
    if (act.get() == nullptr)
      return nullptr;
    auto glob                                     = act->extension<ActivityJavaExt>()->jactivity_;
    act->extension<ActivityJavaExt>()->jactivity_ = nullptr;
    auto local                                    = jenv->NewLocalRef(glob);
    jenv->DeleteGlobalRef(glob);
    return local;
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return nullptr;
}
XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1await_1any_1for(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis,
                                                                                        jdouble timeout)
{
  try {
    ActivityPtr act = ((ActivitySet*)cthis)->wait_any_for(timeout);
    if (act.get() == nullptr)
      return nullptr;
    auto glob                                     = act->extension<ActivityJavaExt>()->jactivity_;
    act->extension<ActivityJavaExt>()->jactivity_ = nullptr;
    auto local                                    = jenv->NewLocalRef(glob);
    jenv->DeleteGlobalRef(glob);
    return local;
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return nullptr;
}
XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1await_1any(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  try {
    ActivityPtr act = ((ActivitySet*)cthis)->wait_any();
    if (act.get() == nullptr)
      return nullptr;
    auto glob                                     = act->extension<ActivityJavaExt>()->jactivity_;
    act->extension<ActivityJavaExt>()->jactivity_ = nullptr;
    auto local                                    = jenv->NewLocalRef(glob);
    jenv->DeleteGlobalRef(glob);
    return local;
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return nullptr;
}
XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1get_1failed_1activity(JNIEnv* jenv, jclass jcls,
                                                                                              jlong cthis,
                                                                                              jobject jthis)
{
  try {
    ActivityPtr act = ((ActivitySet*)cthis)->get_failed_activity();
    if (act.get() == nullptr)
      return nullptr;
    auto glob                                     = act->extension<ActivityJavaExt>()->jactivity_;
    act->extension<ActivityJavaExt>()->jactivity_ = nullptr;
    auto local                                    = jenv->NewLocalRef(glob);
    jenv->DeleteGlobalRef(glob);
    return local;
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return nullptr;
}
XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_ActivitySet_1has_1failed_1activity(JNIEnv* jenv,
                                                                                               jclass jcls, jlong cthis,
                                                                                               jobject jthis)
{
  try {
    return ((ActivitySet*)cthis)->has_failed_activities();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return false;
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1start_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    simgrid::s4u::Exec::on_start_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1this_1start_1cb(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Exec*)cthis)->on_this_start_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1completion_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    simgrid::s4u::Exec::on_completion_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1this_1completion_1cb(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis,
                                                                                       jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Exec*)cthis)->on_this_completion_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1this_1suspend_1cb(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis,
                                                                                    jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Exec*)cthis)->on_this_suspend_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1this_1resume_1cb(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Exec*)cthis)->on_this_resume_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1veto_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    simgrid::s4u::Exec::on_veto_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1on_1this_1veto_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Exec*)cthis)->on_this_veto_cb([cb](Exec const& e) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, &e);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1add_1successor(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jlong cother,
                                                                             jobject jother)
{
  ActivityPtr other = reinterpret_cast<Activity*>(cother);
  auto self = (Exec*)cthis;
  self->add_successor(other);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1remove_1successor(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jlong cother,
                                                                                jobject jother)
{
  auto* self = (Exec*)cthis;
  ActivityPtr other = (Activity*)cother;
  self->remove_successor(other);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jstring jarg2)
{
  auto* self = (Exec*)cthis;
  self->set_name(java_string_to_std_string(jenv, jarg2));
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  const char* result = ((Exec*)cthis)->get_cname();

  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1tracing_1category(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jstring jarg2)
{
  auto self = (Exec*)cthis;
  self->set_tracing_category(java_string_to_std_string(jenv, jarg2));
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1tracing_1category(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis)
{
  std::string result = ((Exec*)cthis)->get_tracing_category();
  return jenv->NewStringUTF(result.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1detach_1_1SWIG_10(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  ((Exec*)cthis)->detach();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1detach_1_1SWIG_11(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Exec*)cthis)->detach([cb](void* exec) {
      get_jenv()->CallVoidMethod(cb, CallbackExec_methodId, exec);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1cancel(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                     jobject jthis)
{
  auto* self = (Exec*)cthis;
  self->cancel();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1await_1for(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis, jdouble jarg2)
{
  auto* self = (Exec*)cthis;
  try {
    self->wait_for(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1start_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Io::on_start_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1this_1start_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Io*)cthis)->on_this_start_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1completion_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Io::on_completion_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1this_1completion_1cb(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Io*)cthis)->on_this_completion_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1this_1suspend_1cb(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Io*)cthis)->on_this_suspend_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1this_1resume_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Io*)cthis)->on_this_resume_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1veto_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Io::on_veto_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1on_1this_1veto_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Io*)cthis)->on_this_veto_cb([cb](Io const& i) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, &i);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1add_1successor(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis, jlong jarg2, jobject jarg2_)
{
  auto* self = (Io*)cthis;
  self->add_successor((Activity*)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1remove_1successor(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis, jlong jarg2,
                                                                              jobject jarg2_)
{
  auto* self = (Io*)cthis;
  self->remove_successor((Activity*)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                      jobject jthis, jstring jarg2)
{
  auto* self = (Io*)cthis;
  self->set_name(java_string_to_std_string(jenv, jarg2));
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  const char* result = ((Io*)cthis)->get_cname();
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1tracing_1category(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jstring jarg2)
{
  auto* self = (Io*)cthis;
  self->set_tracing_category(java_string_to_std_string(jenv, jarg2));
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1get_1tracing_1category(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis)
{
  std::string result = ((Io*)cthis)->get_tracing_category();
  return jenv->NewStringUTF(result.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1detach_1_1SWIG_10(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  ((Io*)cthis)->detach();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1detach_1_1SWIG_11(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Io*)cthis)->detach([cb](void* io) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, io);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1cancel(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                   jobject jthis)
{
  auto* self = (Io*)cthis;
  self->cancel();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1await_1for(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                       jobject jthis, jdouble jarg2)
{
  auto* self = (Io*)cthis;
  try {
    self->wait_for(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Barrier_1create(JNIEnv* jenv, jclass jcls, jlong size)
{
  BarrierPtr result = simgrid::s4u::Barrier::create(size);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Barrier_1to_1string(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  std::string result = ((Barrier*)cthis)->to_string();
  return jenv->NewStringUTF(result.c_str());
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Barrier_1await(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  bool res = false;
  try {
    res = ((Barrier*)cthis)->wait();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return res;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1Barrier(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  intrusive_ptr_release((Barrier*)cthis);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1send_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Comm::on_send_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1this_1send_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->on_this_send_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1recv_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Comm::on_recv_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1this_1recv_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->on_this_recv_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1sendto_1init_1_1SWIG_10(JNIEnv* jenv, jclass jcls)
{
  CommPtr result = simgrid::s4u::Comm::sendto_init();
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1sendto_1init_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cfrom, jobject jfrom,
                                                                                       jlong cto, jobject jto)
{
  CommPtr result = simgrid::s4u::Comm::sendto_init((Host*)cfrom, (Host*)cto);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1sendto_1async(JNIEnv* jenv, jclass jcls, jlong cfrom,
                                                                             jobject jfrom, jlong cto, jobject jto,
                                                                             jlong jarg3)
{
  CommPtr result = simgrid::s4u::Comm::sendto_async((Host*)cfrom, (Host*)cto, jarg3);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1sendto(JNIEnv* jenv, jclass jcls, jlong cfrom,
                                                                     jobject jfrom, jlong cto, jobject jto, jlong jarg3)
{
  try {
    simgrid::s4u::Comm::sendto((Host*)cfrom, (Host*)cto, jarg3);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1source(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis, jlong chost, jobject jhost)
{
  ((Comm*)cthis)->set_source((Host*)chost);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1source(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  return (jlong)((Comm*)cthis)->get_source();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1destination(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jlong chost,
                                                                               jobject jhost)
{
  ((Comm*)cthis)->set_destination((Host*)chost);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1destination(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  return (jlong)((Comm*)cthis)->get_destination();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1mailbox(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis, jlong jarg2, jobject jarg2_)
{
  ((Comm*)cthis)->set_mailbox((Mailbox*)jarg2);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1mailbox(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis)
{
  return (jlong)((Comm*)cthis)->get_mailbox();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1src_1data(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jobject payload)
{
  auto self = (Comm*)cthis;
  auto glob = jenv->NewGlobalRef(payload);

  self->set_src_data(glob);
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1payload(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  auto self    = (Comm*)cthis;
  jobject glob = (jobject)self->get_payload();
  auto local   = jenv->NewLocalRef(glob);
  jenv->DeleteGlobalRef(glob);

  return local;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1payload_1size(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jlong jarg2)
{
  auto self = (Comm*)cthis;
  self->set_payload_size(jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1rate(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jdouble jarg2)
{
  auto self = (Comm*)cthis;
  self->set_rate(jarg2);
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1is_1assigned(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  auto self = (Comm*)cthis;
  return self->is_assigned();
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1sender(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis)
{
  ActorPtr result = ((Comm*)cthis)->get_sender();
  return result.get() != nullptr ? result->extension<ActorJavaExt>()->jactor_ : nullptr;
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1receiver(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  ActorPtr result = ((Comm*)cthis)->get_receiver();
  return result.get() != nullptr ? result->extension<ActorJavaExt>()->jactor_ : nullptr;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1await_1for(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis, jdouble jarg2)
{
  try {
    ((Comm*)cthis)->wait_for(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1start_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Comm::on_start_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1this_1start_1cb(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->on_this_start_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1completion_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Comm::on_completion_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1this_1completion_1cb(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis,
                                                                                       jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->on_this_completion_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1this_1suspend_1cb(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis,
                                                                                    jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->on_this_suspend_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1this_1resume_1cb(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->on_this_resume_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1veto_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Comm::on_veto_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1on_1this_1veto_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->on_this_veto_cb([cb](Comm const& c) {
      get_jenv()->CallVoidMethod(cb, CallbackComm_methodId, &c);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1add_1successor(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jlong jarg2, jobject jarg2_)
{
  auto self = (Comm*)cthis;
  self->add_successor((Activity*)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1remove_1successor(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jlong jarg2,
                                                                                jobject jarg2_)
{
  auto self = (Comm*)cthis;
  self->remove_successor((Activity*)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jstring jarg2)
{
  auto self = (Comm*)cthis;
  self->set_name(java_string_to_std_string(jenv, jarg2));
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  const char* result = ((Comm*)cthis)->get_cname();
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1set_1tracing_1category(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jstring jarg2)
{
  auto self = (Comm*)cthis;
  self->set_tracing_category(java_string_to_std_string(jenv, jarg2));
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1get_1tracing_1category(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis)
{
  std::string result = ((Comm*)cthis)->get_tracing_category();
  return jenv->NewStringUTF(result.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1detach_1_1SWIG_10(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  auto* self = (Comm*)cthis;
  self->detach();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1detach_1_1SWIG_11(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Comm*)cthis)->detach([cb](void* comm) {
      get_jenv()->CallVoidMethod(cb, CallbackIo_methodId, comm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Comm_1cancel(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                     jobject jthis)
{
  ((Comm*)cthis)->cancel();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1ConditionVariable(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis)
{
  intrusive_ptr_release((ConditionVariable*)cthis);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_ConditionVariable_1create(JNIEnv* jenv, jclass jcls)
{
  ConditionVariablePtr result = simgrid::s4u::ConditionVariable::create();
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ConditionVariable_1await_1until(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis,
                                                                                        jlong cmutex, jobject jmutex,
                                                                                        jdouble jarg3)
{
  if (!cmutex) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "the mutex associated to this CV is null");
    return;
  }
  try {
    if (std::cv_status::timeout == ((ConditionVariable*)cthis)->wait_until(((Mutex*)cmutex), jarg3))
      throw TimeoutException(XBT_THROW_POINT, "timeouted!");
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ConditionVariable_1await_1for(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis,
                                                                                      jlong cmutex, jobject jmutex,
                                                                                      jdouble jarg3)
{
  if (!cmutex) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "the mutex associated to this CV is null");
    return;
  }
  try {
    if (std::cv_status::timeout == ((ConditionVariable*)cthis)->wait_for(((Mutex*)cmutex), jarg3))
      throw TimeoutException(XBT_THROW_POINT, "timeouted!");
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ConditionVariable_1notify_1one(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  try {
    ((ConditionVariable*)cthis)->notify_one();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_ConditionVariable_1notify_1all(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  try {
    ((ConditionVariable*)cthis)->notify_all();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1create_1disk(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis, jstring jname,
                                                                            jdouble read_bw, jdouble write_bw)
{
  return (jlong)((Host*)cthis)->create_disk(java_string_to_std_string(jenv, jname), read_bw, write_bw);
}
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1create_1vm(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis, jstring jname,
                                                                          jint core_amount)
{
  return (jlong)((Host*)cthis)->create_vm(java_string_to_std_string(jenv, jname), core_amount);
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1disks(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  auto [disk_class, disk_ctor] = get_classctor_disk(jenv);

  auto cres = ((Host*)cthis)->get_disks();

  jobjectArray jres = jenv->NewObjectArray(cres.size(), disk_class, nullptr);
  for (unsigned int i = 0; i < cres.size(); i++) {
    jenv->SetObjectArrayElement(jres, i, jenv->NewObject(disk_class, disk_ctor, cres.at(i)));
    check_javaexception(jenv);
  }

  return jres;
}
XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1properties_1names(JNIEnv* jenv, jclass jcls,
                                                                                             jlong cthis, jobject jthis)
{
  const std::unordered_map<std::string, std::string>* cproperties = ((Host*)cthis)->get_properties();
  jobjectArray jres = jenv->NewObjectArray(cproperties->size(), string_class, nullptr);
  int i             = 0;
  for (auto [key, _] : *cproperties) {
    jenv->SetObjectArrayElement(jres, i, jenv->NewStringUTF(key.c_str()));
    check_javaexception(jenv);
    i++;
  }
  return jres;
}
XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jstring jprop)
{
  std::string cprop  = java_string_to_std_string(jenv, jprop);
  const char* result = ((Host*)cthis)->get_property(cprop);
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}
XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  const char* result = ((Disk*)cthis)->get_cname();
  if (result)
    return jenv->NewStringUTF(result);
  return nullptr;
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1data(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  jobject cdata = ((Host*)cthis)->get_data<_jobject>();
  if (cdata != nullptr)
    cdata = jenv->NewLocalRef(cdata);
  return cdata;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1data(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jobject jdata)
{
  // Clean any object already stored
  jobject cdata = ((Disk*)cthis)->get_data<_jobject>();
  if (cdata != nullptr)
    jenv->DeleteGlobalRef(cdata);

  // Store the new object
  auto glob = jdata;
  if (glob != nullptr)
    glob = jenv->NewGlobalRef(glob);

  ((Disk*)cthis)->set_data(glob);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1read_1bandwidth(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jdouble jarg2)
{
  ((Disk*)cthis)->set_read_bandwidth(jarg2);
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1read_1bandwidth(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis)
{
  return ((Disk*)cthis)->get_read_bandwidth();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1write_1bandwidth(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis,
                                                                                    jdouble jarg2)
{
  ((Disk*)cthis)->set_write_bandwidth(jarg2);
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1write_1bandwidth(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  return ((Disk*)cthis)->get_write_bandwidth();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1readwrite_1bandwidth(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis,
                                                                                        jdouble jarg2)
{
  ((Disk*)cthis)->set_readwrite_bandwidth(jarg2);
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jstring jprop)
{
  std::string cprop  = java_string_to_std_string(jenv, jprop);
  const char* result = ((Disk*)cthis)->get_property(cprop);
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis, jstring jprop, jstring jval)
{
  if (jprop != nullptr && jval != nullptr) {
    std::string cprop = java_string_to_std_string(jenv, jprop);
    std::string cval  = java_string_to_std_string(jenv, jval);
    ((Disk*)cthis)->set_property(cprop, cval);
  } else if (jprop == nullptr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property name shall not be null.");
  } else {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property value shall not be null.");
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1host(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jlong chost, jobject jhost)
{
  ((Disk*)cthis)->set_host((Host*)chost);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1host(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  return (jlong)((Disk*)cthis)->get_host();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1concurrency_1limit(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis,
                                                                                      jint jarg2)
{
  ((Disk*)cthis)->set_concurrency_limit(jarg2);
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1concurrency_1limit(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis)
{
  return ((Disk*)cthis)->get_concurrency_limit();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1io_1init(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jint jarg2, jint jarg3)
{
  auto self = (Disk*)cthis;

  IoPtr result = self->io_init(jarg2, (Io::OpType)jarg3);
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1read_1async(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis, jint jarg2)
{
  auto self = (Disk*)cthis;

  IoPtr result = self->read_async(jarg2);
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1read_1_1SWIG_10(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis, jint jarg2)
{
  int result = 0;
  try {
    result = ((Disk*)cthis)->read(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return result;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1read_1_1SWIG_11(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis, jint jarg2, jdouble jarg3)
{
  int result = 0;
  try {
    result = ((Disk*)cthis)->read(jarg2, jarg3);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return result;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1write_1async(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis, jint jarg2)
{
  auto self = (Disk*)cthis;

  IoPtr result = self->write_async(jarg2);
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1write_1_1SWIG_10(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jint jarg2)
{
  int result = 0;
  try {
    result = ((Disk*)cthis)->write(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return result;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1write_1_1SWIG_11(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jint jarg2, jdouble jarg3)
{
  int result = 0;
  try {
    result = ((Disk*)cthis)->write(jarg2, jarg3);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return result;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1SharingPolicy_1NONLINEAR_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)Disk::SharingPolicy::NONLINEAR;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1SharingPolicy_1LINEAR_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)Disk::SharingPolicy::LINEAR;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1Operation_1READ_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Disk::Operation::READ;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1Operation_1WRITE_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Disk::Operation::WRITE;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1Operation_1READWRITE_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Disk::Operation::READWRITE;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1get_1sharing_1policy(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jint jarg2)
{
  return (jint)((Disk*)cthis)->get_sharing_policy((simgrid::s4u::Disk::Operation)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1set_1factor_1cb(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis, jlong jarg2)
{
  std::function<simgrid::s4u::Disk::IoFactorCb>* arg2 = 0;

  arg2 = *(std::function<simgrid::s4u::Disk::IoFactorCb>**)&jarg2;
  if (!arg2) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException,
                            "std::function< simgrid::s4u::Disk::IoFactorCb > const & is null");
    return;
  }
  ((Disk*)cthis)->set_factor_cb((std::function<simgrid::s4u::Disk::IoFactorCb> const&)*arg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1seal(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                   jobject jthis)
{
  ((Disk*)cthis)->seal();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1onoff_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Disk::on_onoff_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1this_1onoff_1cb(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Disk*)cthis)->on_this_onoff_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1read_1bandwidth_1change_1cb(JNIEnv* jenv, jclass jcls,
                                                                                              jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Disk::on_read_bandwidth_change_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1this_1read_1bandwidth_1change_1cb(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Disk*)cthis)->on_this_read_bandwidth_change_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1write_1bandwidth_1change_1cb(JNIEnv* jenv,
                                                                                               jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Disk::on_write_bandwidth_change_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1this_1write_1bandwidth_1change_1cb(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Disk*)cthis)->on_this_write_bandwidth_change_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1destruction_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Disk::on_destruction_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Disk_1on_1this_1destruction_1cb(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis,
                                                                                        jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Disk*)cthis)->on_this_destruction_cb([cb](Disk const& disk) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &disk);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

jobjectArray cleaned_args;
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_new_1Engine(JNIEnv* jenv, jclass jcls, jobjectArray jargs)
{
  check_nullparam(jargs, "The engine arguments shall not be null");
  int len = (int)jenv->GetArrayLength(jargs);
  if (len < 0) {
    SWIG_JavaThrowException(jenv, SWIG_JavaRuntimeException, "array length negative");
    return 0;
  }
  char** cargs = (char**)malloc((len + 2) * sizeof(char*)); // +1 for final NULL ; +1 for initial "java"
  if (cargs == NULL) {
    SWIG_JavaThrowException(jenv, SWIG_JavaOutOfMemoryError, "memory allocation failed");
    return 0;
  }

  cargs[0] = xbt_strdup("java"); // SimGrid expects argv[0] to be useless
  for (jsize i = 0; i < len; i++) {
    jstring j_string     = (jstring)jenv->GetObjectArrayElement(jargs, i);
    const char* c_string = jenv->GetStringUTFChars(j_string, 0);
    cargs[i + 1]         = (char*)c_string;
  }
  cargs[len + 1] = NULL;
  len++;

  auto* result = new simgrid::s4u::Engine(&len, cargs);

  /* Reallocate the args now that SimGrid just removed its parameters */
  cleaned_args = jenv->NewObjectArray(len - 1, string_class, nullptr);

  for (int i = 1; cargs[i] != nullptr; i++) {
    jenv->SetObjectArrayElement(cleaned_args, i - 1, jenv->NewStringUTF(cargs[i]));
    check_javaexception(jenv);
  }

  free(cargs);
  return (jlong)result;
}
XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1args(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis)
{
  return cleaned_args;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1run(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                    jobject jthis)
{
  ((Engine*)cthis)->run();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1run_1until(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis, jdouble jarg2)
{
  ((Engine*)cthis)->run_until(jarg2);
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1clock(JNIEnv* jenv, jclass jcls)
{
  return Engine::get_clock();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1instance(JNIEnv* jenv, jclass jcls)
{
  return (jlong)simgrid::s4u::Engine::get_instance();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1load_1platform(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jstring jfilename)
{
  if (jfilename) {
    std::string cfilename = java_string_to_std_string(jenv, jfilename);
    ((Engine*)cthis)->load_platform(cfilename);
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "The platform file name shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1seal_1platform(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  ((Engine*)cthis)->seal_platform();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1flatify_1platform(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis)
{
  std::string result = ((Engine*)cthis)->flatify_platform();
  return jenv->NewStringUTF(result.c_str());
}

std::set<simgrid::s4u::Activity*> vetoed_activities;
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1track_1vetoed_1activities(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis)
{
  ((Engine*)cthis)->track_vetoed_activities(&vetoed_activities);
}
JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1clear_1vetoed_1activities(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis)
{
  vetoed_activities.clear();
}
JNIEXPORT jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1vetoed_1activities(JNIEnv* jenv,
                                                                                               jclass jcls, jlong cthis)
{
  auto [activity_class, activity_ctor] = get_classctor_activity(jenv);
  auto [comm_class, comm_ctor]         = get_classctor_comm(jenv);
  auto [io_class, io_ctor]             = get_classctor_io(jenv);
  auto [exec_class, exec_ctor]         = get_classctor_exec(jenv);

  jobjectArray jres = jenv->NewObjectArray(vetoed_activities.size(), activity_class, nullptr);

  int i = 0;
  for (Activity* act : vetoed_activities) {
    intrusive_ptr_add_ref(act);
    if (dynamic_cast<Comm*>(act)) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(comm_class, comm_ctor, act, (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Io*>(act)) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(io_class, io_ctor, act, (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Exec*>(act)) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(exec_class, exec_ctor, act, (jboolean)1));
      check_javaexception(jenv);
    } else
      THROW_IMPOSSIBLE;
    i++;
  }
  return jres;
}

/** Create a Java org.simgrid.s4u.Actor of the given subclass and using the (String,String,String[]) constructor */
static void java_main(int argc, char* argv[])
{
  JNIEnv* jenv = get_jenv();

  // Change the "." in class name for "/".
  std::string arg0 = argv[0];
  std::replace(begin(arg0), end(arg0), '.', '/');
  jclass actor_class = jenv->FindClass(arg0.c_str());
  // Retrieve the methodID for the constructor
  xbt_assert((actor_class != nullptr),
             "Class not found (%s). The deployment file must use the fully qualified class name, including the "
             "package. The upper/lower case is important.",
             argv[0]);
  jmethodID actor_constructor = jenv->GetMethodID(actor_class, "<init>", "([Ljava/lang/String;)V");
  xbt_assert(actor_constructor != nullptr,
             "Constructor %s(String[] args) not found. Is there such a constructor in your class?", argv[0]);

  // Build the arguments
  jobjectArray args = jenv->NewObjectArray(argc - 1, string_class, nullptr);
  for (int i = 1; i < argc; i++) {
    jenv->SetObjectArrayElement(args, i - 1, jenv->NewStringUTF(argv[i]));
    check_javaexception(jenv);
  }

  // creates the java actor
  jobject jactor = jenv->NewGlobalRef(jenv->NewObject(actor_class, actor_constructor, args));

  ActorPtr result = Engine::get_instance()->add_actor(argv[0], simgrid::s4u::Host::current(), [jactor]() {
    auto jenv = ((simgrid::kernel::context::JavaContext*)simgrid::kernel::context::Context::self())->jenv_;

    jenv->CallVoidMethod(jactor, Actor_methodId, (jlong)s4u::Actor::self());
    exception_check_after_upcall(get_jenv());
  });
  result->extension_set<ActorJavaExt>(new ActorJavaExt(jactor));
  intrusive_ptr_add_ref(result.get());

  handle_exception(jenv);
  xbt_assert((jactor != nullptr), "Actor creation failed.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1load_1deployment(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jstring jfilename)
{
  if (jfilename) {
    simgrid_register_default(java_main);
    std::string cfilename = java_string_to_std_string(jenv, jfilename);
    ((Engine*)cthis)->load_deployment(cfilename);
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "The deployment file name shall not be null.");
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1host_1count(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis)
{
  return (jlong)((Engine*)cthis)->get_host_count();
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1all_1hosts(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis)
{
  auto [host_class, host_ctor] = get_classctor_host(jenv);

  auto chosts = ((Engine*)cthis)->get_all_hosts();

  jobjectArray result = jenv->NewObjectArray(chosts.size(), host_class, nullptr);
  for (unsigned i = 0; i < chosts.size(); i++) {
    jenv->SetObjectArrayElement(result, i, jenv->NewObject(host_class, host_ctor, chosts.at(i)));
    check_javaexception(jenv);
  }

  return result;
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1hosts_1from_1MPI_1hostfile(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jstring jhostfile)
{
  if (!jhostfile) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "The hostfile shall not be null");
    return 0;
  }

  auto [host_class, host_ctor] = get_classctor_host(jenv);

  auto hostfile = java_string_to_std_string(jenv, jhostfile);
  auto chosts   = ((Engine*)cthis)->get_hosts_from_MPI_hostfile(hostfile);

  jobjectArray result = jenv->NewObjectArray(chosts.size(), host_class, nullptr);
  for (unsigned i = 0; i < chosts.size(); i++) {
    jenv->SetObjectArrayElement(result, i, jenv->NewObject(host_class, host_ctor, chosts.at(i)));
    check_javaexception(jenv);
  }

  return result;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1host_1by_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jstring jname)
{
  try {
    if (jname) {
      std::string name = java_string_to_std_string(jenv, jname);
      return (jlong)((Engine*)cthis)->host_by_name(name);
    }
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Host names shall not be null.");
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1link_1count(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis)
{
  return (jlong)((Engine*)cthis)->get_link_count();
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1all_1links(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis)
{
  auto [link_class, link_ctor] = get_classctor_link(jenv);

  auto clinks = ((Engine*)cthis)->get_all_links();

  jobjectArray result = jenv->NewObjectArray(clinks.size(), link_class, nullptr);
  for (unsigned i = 0; i < clinks.size(); i++) {
    jenv->SetObjectArrayElement(result, i, jenv->NewObject(link_class, link_ctor, clinks.at(i)));
    check_javaexception(jenv);
  }

  return result;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1link_1by_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jstring jname)
{
  try {
    if (jname) {
      std::string name = java_string_to_std_string(jenv, jname);
      return (jlong)((Engine*)cthis)->link_by_name(name);
    }
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Link names shall not be null.");
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1mailbox_1by_1name_1or_1create(JNIEnv* jenv,
                                                                                               jclass jcls, jlong cthis,
                                                                                               jobject jthis,
                                                                                               jstring jname)
{
  if (jname) {
    std::string name = java_string_to_std_string(jenv, jname);
    return (jlong)((Engine*)cthis)->mailbox_by_name_or_create(name);
  }
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Mailbox names shall not be null.");
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1message_1queue_1by_1name_1or_1create(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jstring jname)
{
  if (jname) {
    std::string name = java_string_to_std_string(jenv, jname);
    return (jlong)((Engine*)cthis)->message_queue_by_name_or_create(name);
  }
  SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Message queue names shall not be null.");
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1actor_1count(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  return ((Engine*)cthis)->get_actor_count();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1actor_1max_1pid(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis)
{
  return (jint)((Engine*)cthis)->get_actor_max_pid();
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1all_1actors(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis, jobject jthis)
{
  auto cactors = ((Engine*)cthis)->get_all_actors();

  jobjectArray result = jenv->NewObjectArray(cactors.size(), actor_class, nullptr);
  for (unsigned i = 0; i < cactors.size(); i++) {
    jenv->SetObjectArrayElement(result, i, cactors.at(i)->extension<ActorJavaExt>()->jactor_);
    check_javaexception(jenv);
  }

  return result;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1netzone_1root(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis)
{
  return (jlong)((Engine*)cthis)->get_netzone_root();
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1get_1all_1netzones(JNIEnv* jenv, jclass jcls,
                                                                                           jlong cthis, jobject jthis)
{
  auto [netzone_class, netzone_ctor] = get_classctor_netzone(jenv);

  auto cnetzones = ((Engine*)cthis)->get_all_netzones();

  jobjectArray result = jenv->NewObjectArray(cnetzones.size(), netzone_class, nullptr);
  for (unsigned i = 0; i < cnetzones.size(); i++) {
    jenv->SetObjectArrayElement(result, i, jenv->NewObject(netzone_class, netzone_ctor, cnetzones.at(i)));
    check_javaexception(jenv);
  }

  return result;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1netzone_1by_1name_1or_1null(JNIEnv* jenv, jclass jcls,
                                                                                             jlong cthis, jobject jthis,
                                                                                             jstring jname)
{
  try {
    if (jname) {
      std::string name = java_string_to_std_string(jenv, jname);
      return (jlong)((Engine*)cthis)->netzone_by_name_or_null(name);
    }
      SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Link names shall not be null.");
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1set_1config_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                       jstring jstr)
{
  if (!jstr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "null string");
    return;
  }
  std::string str = java_string_to_std_string(jenv, jstr);
  simgrid::s4u::Engine::set_config(str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1set_1config_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                       jstring jname, jint jarg2)
{
  if (!jname) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "null string");
    return;
  }
  std::string str = java_string_to_std_string(jenv, jname);
  simgrid::s4u::Engine::set_config(str.c_str(), (int)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1set_1config_1_1SWIG_12(JNIEnv* jenv, jclass jcls,
                                                                                       jstring jname, jboolean jarg2)
{
  if (!jname) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "null string");
    return;
  }
  std::string str = java_string_to_std_string(jenv, jname);
  simgrid::s4u::Engine::set_config(str.c_str(), (bool)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1set_1config_1_1SWIG_13(JNIEnv* jenv, jclass jcls,
                                                                                       jstring jname, jdouble jarg2)
{
  if (!jname) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "null string");
    return;
  }
  std::string str = java_string_to_std_string(jenv, jname);
  simgrid::s4u::Engine::set_config(str.c_str(), (double)jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1set_1config_1_1SWIG_14(JNIEnv* jenv, jclass jcls,
                                                                                       jstring jname, jstring jval)
{
  if (!jname || !jval) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "null string");
    return;
  }
  std::string name = java_string_to_std_string(jenv, jname);
  std::string val  = java_string_to_std_string(jenv, jval);
  simgrid::s4u::Engine::set_config(name, val);
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1plugin_1vm_1live_1migration_1init(JNIEnv* jenv,
                                                                                                  jclass jcls,
                                                                                                  jlong cthis)
{
  sg_vm_live_migration_plugin_init();
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1plugin_1host_1energy_1init(JNIEnv* jenv, jclass jcls,
                                                                                           jlong cthis)
{
  sg_host_energy_plugin_init();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1plugin_1link_1energy_1init(JNIEnv* jenv, jclass jcls,
                                                                                           jlong cthis)
{
  sg_link_energy_plugin_init();
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1plugin_1wifi_1energy_1init(JNIEnv* jenv, jclass jcls,
                                                                                           jlong cthis)
{
  sg_wifi_energy_plugin_init();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1on_1platform_1created_1cb(JNIEnv* jenv, jclass jcls,
                                                                                          jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Engine::on_platform_created_cb([cb]() {
      get_jenv()->CallVoidMethod(cb, CallbackVoid_methodId);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1on_1platform_1creation_1cb(JNIEnv* jenv, jclass jcls,
                                                                                           jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Engine::on_platform_creation_cb([cb]() {
      get_jenv()->CallVoidMethod(cb, CallbackVoid_methodId);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1on_1simulation_1start_1cb(JNIEnv* jenv, jclass jcls,
                                                                                          jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Engine::on_simulation_start_cb([cb]() {
      get_jenv()->CallVoidMethod(cb, CallbackVoid_methodId);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1on_1simulation_1end_1cb(JNIEnv* jenv, jclass jcls,
                                                                                        jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Engine::on_simulation_end_cb([cb]() {
      get_jenv()->CallVoidMethod(cb, CallbackVoid_methodId);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1on_1time_1advance_1cb(JNIEnv* jenv, jclass jcls,
                                                                                      jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Engine::on_time_advance_cb([cb](double d) {
      get_jenv()->CallVoidMethod(cb, CallbackDouble_methodId, d);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1on_1deadlock_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Engine::on_deadlock_cb([cb]() {
      get_jenv()->CallVoidMethod(cb, CallbackVoid_methodId);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1die(JNIEnv* jenv, jclass jcls, jstring jstr)
{
  std::string str = java_string_to_std_string(jenv, jstr);
  xbt_die("%s", str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1critical(JNIEnv* jenv, jclass jcls, jstring jstr)
{
  std::string str = java_string_to_std_string(jenv, jstr);
  XBT_CRITICAL("%s", str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1error(JNIEnv* jenv, jclass jcls, jstring jstr)
{
  std::string str = java_string_to_std_string(jenv, jstr);
  XBT_ERROR("%s", str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1warn(JNIEnv* jenv, jclass jcls, jstring jstr)
{
  std::string str = java_string_to_std_string(jenv, jstr);
  XBT_WARN("%s", str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1info(JNIEnv* jenv, jclass jcls, jstring jstr)
{
  std::string str = java_string_to_std_string(jenv, jstr);
  XBT_INFO("%s", str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1verbose(JNIEnv* jenv, jclass jcls, jstring jstr)
{
  std::string str = java_string_to_std_string(jenv, jstr);
  XBT_VERB("%s", str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Engine_1debug(JNIEnv* jenv, jclass jcls, jstring jstr)
{
  std::string str = java_string_to_std_string(jenv, jstr);
  XBT_DEBUG("%s", str.c_str());
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1Engine(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  delete ((Engine*)cthis);
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_create_1DAG_1from_1dot(JNIEnv* jenv, jclass jcls,
                                                                                       jstring jstr)
{
  auto [activity_class, activity_ctor] = get_classctor_activity(jenv);
  auto [comm_class, comm_ctor]         = get_classctor_comm(jenv);
  auto [io_class, io_ctor]             = get_classctor_io(jenv);
  auto [exec_class, exec_ctor]         = get_classctor_exec(jenv);

  if (!jstr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Dot content shall not be null");
    return 0;
  }
  std::string str = java_string_to_std_string(jenv, jstr);
  auto cres       = simgrid::s4u::create_DAG_from_dot(str);

  jobjectArray jres = jenv->NewObjectArray(cres.size(), activity_class, nullptr);

  int i             = 0;
  for (ActivityPtr const& act : cres) {
    intrusive_ptr_add_ref(act.get());
    if (dynamic_cast<Comm*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(comm_class, comm_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Io*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(io_class, io_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Exec*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(exec_class, exec_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else
      THROW_IMPOSSIBLE;
    i++;
  }
  return jres;
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_create_1DAG_1from_1DAX(JNIEnv* jenv, jclass jcls,
                                                                                       jstring jstr)
{
  auto [activity_class, activity_ctor] = get_classctor_activity(jenv);
  auto [comm_class, comm_ctor]         = get_classctor_comm(jenv);
  auto [io_class, io_ctor]             = get_classctor_io(jenv);
  auto [exec_class, exec_ctor]         = get_classctor_exec(jenv);

  if (!jstr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "DAX content shall not be null");
    return 0;
  }
  std::string str = java_string_to_std_string(jenv, jstr);
  auto cres       = simgrid::s4u::create_DAG_from_DAX(str);

  jobjectArray jres = jenv->NewObjectArray(cres.size(), activity_class, nullptr);

  int i             = 0;
  for (ActivityPtr const& act : cres) {
    intrusive_ptr_add_ref(act.get());
    if (dynamic_cast<Comm*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(comm_class, comm_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Io*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(io_class, io_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Exec*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(exec_class, exec_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else
      THROW_IMPOSSIBLE;
    i++;
  }
  return jres;
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_create_1DAG_1from_1json(JNIEnv* jenv, jclass jcls,
                                                                                        jstring jstr)
{
  auto [activity_class, activity_ctor] = get_classctor_activity(jenv);
  auto [comm_class, comm_ctor]         = get_classctor_comm(jenv);
  auto [io_class, io_ctor]             = get_classctor_io(jenv);
  auto [exec_class, exec_ctor]         = get_classctor_exec(jenv);

  if (!jstr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "JSON content shall not be null");
    return 0;
  }
  std::string str = java_string_to_std_string(jenv, jstr);
  auto cres       = simgrid::s4u::create_DAG_from_json(str);

  jobjectArray jres = jenv->NewObjectArray(cres.size(), activity_class, nullptr);

  int i             = 0;
  for (ActivityPtr const& act : cres) {
    intrusive_ptr_add_ref(act.get());
    if (dynamic_cast<Comm*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(comm_class, comm_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Io*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(io_class, io_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else if (dynamic_cast<Exec*>(act.get())) {
      jenv->SetObjectArrayElement(jres, i, jenv->NewObject(exec_class, exec_ctor, act.get(), (jboolean)1));
      check_javaexception(jenv);
    } else
      THROW_IMPOSSIBLE;
    i++;
  }
  return jres;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1init(JNIEnv* jenv, jclass jcls)
{
  ExecPtr result = simgrid::s4u::Exec::init();
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1remaining(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  return ((Exec*)cthis)->get_remaining();
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1remaining_1ratio(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  return ((Exec*)cthis)->get_remaining_ratio();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1host(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jlong chost, jobject jhost)
{
  ((Exec*)cthis)->set_host((Host*)chost);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1hosts(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis, jlongArray jhosts)
{
  std::vector<Host*> chosts;
  int len       = jenv->GetArrayLength(jhosts);
  jlong* cjhosts = jenv->GetLongArrayElements(jhosts, nullptr);
  for (int i = 0; i < len; i++)
    chosts.push_back((Host*)cjhosts[i]);
  jenv->ReleaseLongArrayElements(jhosts, cjhosts, JNI_ABORT);

  ((Exec*)cthis)->set_hosts(chosts);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1unset_1host(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis)
{
  ((Exec*)cthis)->unset_host();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1unset_1hosts(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  ((Exec*)cthis)->unset_hosts();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1flops_1amount(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jdouble jarg2)
{
  ((Exec*)cthis)->set_flops_amount(jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1flops_1amounts(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jdoubleArray jamounts)
{
  if (!jamounts) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Array of amounts shall not be null");
    return;
  }

  std::vector<double> camounts;
  int len            = jenv->GetArrayLength(jamounts);
  jdouble* cjamounts = jenv->GetDoubleArrayElements(jamounts, nullptr);
  for (int i = 0; i < len; i++)
    camounts.push_back((double)cjamounts[i]);
  jenv->ReleaseDoubleArrayElements(jamounts, cjamounts, JNI_ABORT);

  ((Exec*)cthis)->set_flops_amounts(camounts);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1bytes_1amounts(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jdoubleArray jamounts)
{
  if (!jamounts) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Array of amounts shall not be null");
    return;
  }

  std::vector<double> camounts;
  int len            = jenv->GetArrayLength(jamounts);
  jdouble* cjamounts = jenv->GetDoubleArrayElements(jamounts, nullptr);
  for (int i = 0; i < len; i++)
    camounts.push_back((double)cjamounts[i]);
  jenv->ReleaseDoubleArrayElements(jamounts, cjamounts, JNI_ABORT);

  ((Exec*)cthis)->set_bytes_amounts(camounts);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1thread_1count(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis, jint jarg2)
{
  ((Exec*)cthis)->set_thread_count(jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1bound(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis, jdouble jarg2)
{
  ((Exec*)cthis)->set_bound(jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1set_1priority(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis, jdouble jarg2)
{
  ((Exec*)cthis)->set_priority(jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1update_1priority(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jdouble jarg2)
{
  ((Exec*)cthis)->update_priority(jarg2);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1host(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  return (jlong)((Exec*)cthis)->get_host();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1host_1number(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  return ((Exec*)cthis)->get_host_number();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1thread_1count(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  return ((Exec*)cthis)->get_thread_count();
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1get_1cost(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  return ((Exec*)cthis)->get_cost();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1is_1parallel(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  return ((Exec*)cthis)->is_parallel();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Exec_1is_1assigned(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  return ((Exec*)cthis)->is_assigned();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1current(JNIEnv* jenv, jclass jcls)
{
  return (jlong)Host::current();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  const char* result = ((Host*)cthis)->get_cname();
  if (result)
    return jenv->NewStringUTF(result);
  return nullptr;
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1speed(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis)
{
  return (jdouble)((Host*)cthis)->get_speed();
}
XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1pstate_1count(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  return (jint)((Host*)cthis)->get_pstate_count();
}
XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1pstate_1speed(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis,
                                                                                    jint pstate)
{
  return (jdouble)((Host*)cthis)->get_pstate_speed(pstate);
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1load(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  return (jdouble)((Host*)cthis)->get_load();
}
XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1is_1on(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  return ((simgrid::s4u::Host*)cthis)->is_on();
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1turn_1on(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  return ((simgrid::s4u::Host*)cthis)->turn_on();
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1turn_1off(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  return ((simgrid::s4u::Host*)cthis)->turn_off();
}
JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1set_1pstate(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jint pstate)
{
  ((simgrid::s4u::Host*)cthis)->set_pstate(pstate);
}
JNIEXPORT jint JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1pstate(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  return ((simgrid::s4u::Host*)cthis)->get_pstate();
}
JNIEXPORT jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1exec_1init(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jdouble flops)
{
  ExecPtr result = ((simgrid::s4u::Host*)cthis)->exec_init(flops);
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}
JNIEXPORT jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1exec_1async(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jdouble flops)
{
  ExecPtr result = ((simgrid::s4u::Host*)cthis)->exec_async(flops);
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1data(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  jobject cdata = ((Host*)cthis)->get_data<_jobject>();
  if (cdata != nullptr)
    cdata = jenv->NewLocalRef(cdata);
  return cdata;
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1set_1data(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject obj)
{
  // Clean any object already stored
  jobject cdata = ((Host*)cthis)->get_data<_jobject>();
  if (cdata != nullptr)
    jenv->DeleteGlobalRef(cdata);

  // Store the new object
  auto glob = obj;
  if (glob != nullptr)
    glob = jenv->NewGlobalRef(obj);

  ((Host*)cthis)->set_data(glob);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1set_1concurrency_1limit(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jint limit)
{
  ((Host*)cthis)->set_concurrency_limit(limit);
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1set_1cpu_1factor_1cb(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject cb)
{
  if (cb) {
    cb                                                = jenv->NewGlobalRef(cb);
    const std::function<double(Host&, double)> lambda = [cb](Host const& h, double flops) -> double {
      double res = get_jenv()->CallDoubleMethod(cb, CallbackDHostDouble_methodId, &h, flops);
      exception_check_after_upcall(get_jenv());
      return res;
    };

    ((Host*)cthis)->set_cpu_factor_cb(lambda);
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}
XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1consumed_1energy(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis)
{
  return sg_host_get_consumed_energy((Host*)cthis);
}
XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1wattmax_1at(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jint pstate)
{
  return sg_host_get_wattmax_at((Host*)cthis, pstate);
}
XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Host_1get_1wattmin_1at(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jint pstate)
{
  return sg_host_get_wattmin_at((Host*)cthis, pstate);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1init(JNIEnv* jenv, jclass jcls)
{
  try {
    IoPtr result = simgrid::s4u::Io::init();
    intrusive_ptr_add_ref(result.get());
    return (jlong)result.get();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1get_1remaining(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  return ((Io*)cthis)->get_remaining();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1get_1performed_1ioops(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis)
{
  return ((Io*)cthis)->get_performed_ioops();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1disk(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                      jobject jthis, jlong cdisk, jobject jdisk)
{
  ((Io*)cthis)->set_disk((Disk*)cdisk);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1priority(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis, jdouble jarg2)
{
  ((Io*)cthis)->set_priority(jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1size(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                      jobject jthis, jint jarg2)
{
  ((Io*)cthis)->set_size(jarg2);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1op_1type(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                          jobject jthis, jint jarg2)
{
  ((Io*)cthis)->set_op_type((simgrid::s4u::Io::OpType)jarg2);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1streamto_1init(JNIEnv* jenv, jclass jcls, jlong chost_from,
                                                                            jobject jthis, jlong cdisk_from,
                                                                            jobject jarg2_, jlong chost_to,
                                                                            jobject jarg3_, jlong cdisk_to,
                                                                            jobject jdisk_to)
{
  try {
    IoPtr result =
        simgrid::s4u::Io::streamto_init((Host*)chost_from, (Disk*)cdisk_from, (Host*)chost_to, (Disk*)cdisk_to);
    intrusive_ptr_add_ref(result.get());
    return (jlong)result.get();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1streamto_1async(
    JNIEnv* jenv, jclass jcls, jlong chost_from, jobject jhost_from, jlong cdisk_from, jobject jdisk_from,
    jlong chost_to, jobject jhost_to, jlong cdisk_to, jobject jdisk_to, jlong simulated_size_in_bytes)
{
  try {
    IoPtr result = simgrid::s4u::Io::streamto_async((Host*)chost_from, (Disk*)cdisk_from, (Host*)chost_to,
                                                    (Disk*)cdisk_to, simulated_size_in_bytes);
    intrusive_ptr_add_ref(result.get());
    return (jlong)result.get();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1streamto(JNIEnv* jenv, jclass jcls, jlong chost_from,
                                                                     jobject jhost_from, jlong cdisk_from,
                                                                     jobject jdisk_from, jlong chost_to,
                                                                     jobject jhost_to, jlong cdisk_to, jobject jdisk_to,
                                                                     jlong simulated_size_in_bytes)
{
  try {
    simgrid::s4u::Io::streamto((Host*)chost_from, (Disk*)cdisk_from, (Host*)chost_to, (Disk*)cdisk_to,
                               simulated_size_in_bytes);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1source(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis, jlong chost, jobject jarg2_,
                                                                        jlong cdisk, jobject jarg3_)
{
  ((Io*)cthis)->set_source((Host*)chost, (Disk*)cdisk);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1set_1destination(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jlong chost, jobject jarg2_,
                                                                             jlong cdisk, jobject jarg3_)
{
  ((Io*)cthis)->set_destination((Host*)chost, (Disk*)cdisk);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1update_1priority(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jdouble jarg2)
{
  ((Io*)cthis)->update_priority(jarg2);
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Io_1is_1assigned(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis)
{
  return ((Io*)cthis)->is_assigned();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1SharingPolicy_1NONLINEAR_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Link::SharingPolicy::NONLINEAR;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1SharingPolicy_1WIFI_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Link::SharingPolicy::WIFI;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1SharingPolicy_1SPLITDUPLEX_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Link::SharingPolicy::SPLITDUPLEX;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1SharingPolicy_1SHARED_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Link::SharingPolicy::SHARED;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1SharingPolicy_1FATPIPE_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::Link::SharingPolicy::FATPIPE;
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  try {
    simgrid::s4u::Link* arg1 = (simgrid::s4u::Link*)cthis;
    const char* result       = (arg1)->get_cname();
    if (result)
      return jenv->NewStringUTF(result);
  } catch (ForcefulKillException const&) { /* Actor killed */
  }
  return nullptr;
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1get_1bandwidth(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  try {
    return (jdouble)((simgrid::s4u::Link*)cthis)->get_bandwidth();
  } catch (ForcefulKillException const&) {
    return 0;
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1set_1bandwidth(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jdouble jarg2)
{
  try {
    ((Link*)cthis)->set_bandwidth(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed */
  }
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1get_1latency(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  try {
    return ((Link*)cthis)->get_bandwidth();
  } catch (ForcefulKillException const&) {
    return 0;
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1set_1latency_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis,
                                                                                      jdouble jarg2)
{
  try {
    ((Link*)cthis)->set_latency(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1set_1latency_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis,
                                                                                      jstring jstr)
{
  if (jstr) {
    std::string str = java_string_to_std_string(jenv, jstr);
    try {
      ((Link*)cthis)->set_latency(str.c_str());
    } catch (ForcefulKillException const&) { /* Actor killed */
    }
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Latency value shall not be null.");
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1get_1sharing_1policy(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  try {
    return (jint)((Link*)cthis)->get_sharing_policy();
  } catch (ForcefulKillException const&) {
    return 0;
  }
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1get_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jstring jprop)
{
  std::string cprop  = java_string_to_std_string(jenv, jprop);
  try {
    const char* result = ((Link*)cthis)->get_property(cprop);
    if (result)
      return jenv->NewStringUTF(result);
  } catch (ForcefulKillException const&) { /* Actor killed */
  }
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1set_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis, jstring jprop, jstring jval)
{
  if (jprop != nullptr && jval != nullptr) {
    std::string cprop = java_string_to_std_string(jenv, jprop);
    std::string cval  = java_string_to_std_string(jenv, jval);
    try {
      ((Link*)cthis)->set_property(cprop, cval);
    } catch (ForcefulKillException const&) { /* Actor killed */
    }
  } else if (jprop == nullptr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property name shall not be null.");
  } else {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property value shall not be null.");
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1set_1concurrency_1limit(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis,
                                                                                      jint jarg2)
{
  try {
    ((Link*)cthis)->set_concurrency_limit(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed */
  }
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1get_1concurrency_1limit(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis)
{
  try {
    return (jint)((Link*)cthis)->get_concurrency_limit();
  } catch (ForcefulKillException const&) {
    return 0;
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1set_1host_1wifi_1rate(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis,
                                                                                    jlong chost, jobject jhost,
                                                                                    jint jarg3)
{
  try {
    ((Link*)cthis)->set_host_wifi_rate((Host*)chost, jarg3);
  } catch (ForcefulKillException const&) { /* Actor killed */
  }
}

XBT_PUBLIC jdouble JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1get_1load(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  try {
    return ((Link*)cthis)->get_load();
  } catch (ForcefulKillException const&) {
    return 0;
  }
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1is_1used(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  try {
    return ((Link*)cthis)->is_used();
  } catch (ForcefulKillException const&) {
    return false;
  }
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1is_1shared(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis)
{
  try {
    return ((Link*)cthis)->is_shared();
  } catch (ForcefulKillException const&) {
    return false;
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1turn_1on(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                       jobject jthis)
{
  try {
    ((Link*)cthis)->turn_on();
  } catch (ForcefulKillException const&) { /* Actor killed. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1turn_1off(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis)
{
  try {
    ((Link*)cthis)->turn_off();
  } catch (ForcefulKillException const&) { /* Actor killed. */
  }
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1is_1on(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                         jobject jthis)
{
  try {
    return ((Link*)cthis)->is_on();
  } catch (ForcefulKillException const&) { /* Actor killed. */
  }
  return false;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1seal(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                   jobject jthis)
{
  try {
    ((Link*)cthis)->seal();
  } catch (ForcefulKillException const&) { /* Actor killed. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1on_1onoff_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Link::on_onoff_cb([cb](Link const& l) {
      get_jenv()->CallVoidMethod(cb, CallbackLink_methodId, &l);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1on_1this_1onoff_1cb(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Link*)cthis)->on_this_onoff_cb([cb](Link const& l) {
      get_jenv()->CallVoidMethod(cb, CallbackLink_methodId, &l);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1on_1bandwidth_1change_1cb(JNIEnv* jenv, jclass jcls,
                                                                                        jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Link::on_bandwidth_change_cb([cb](Link const& l) {
      get_jenv()->CallVoidMethod(cb, CallbackLink_methodId, &l);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1on_1this_1bandwidth_1change_1cb(JNIEnv* jenv, jclass jcls,
                                                                                              jlong cthis,
                                                                                              jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Link*)cthis)->on_this_bandwidth_change_cb([cb](Link const& l) {
      get_jenv()->CallVoidMethod(cb, CallbackLink_methodId, &l);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1on_1destruction_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    Link::on_destruction_cb([cb](Link const& l) {
      get_jenv()->CallVoidMethod(cb, CallbackLink_methodId, &l);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Link_1on_1this_1destruction_1cb(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis,
                                                                                        jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((Link*)cthis)->on_this_destruction_cb([cb](Link const& l) {
      get_jenv()->CallVoidMethod(cb, CallbackLink_methodId, &l);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_LinkInRoute_1Direction_1UP_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::LinkInRoute::Direction::UP;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_LinkInRoute_1Direction_1DOWN_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::LinkInRoute::Direction::DOWN;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_LinkInRoute_1Direction_1NONE_1get(JNIEnv* jenv, jclass jcls)
{
  return (jint)simgrid::s4u::LinkInRoute::Direction::NONE;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_new_1LinkInRoute_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis)
{
  jlong jresult                     = 0;
  simgrid::s4u::Link* arg1          = (simgrid::s4u::Link*)0;
  simgrid::s4u::LinkInRoute* result = 0;

  (void)jenv;
  (void)jcls;
  (void)jthis;
  arg1   = *(simgrid::s4u::Link**)&cthis;
  result = (simgrid::s4u::LinkInRoute*)new simgrid::s4u::LinkInRoute((simgrid::s4u::Link const*)arg1);
  *(simgrid::s4u::LinkInRoute**)&jresult = result;
  return jresult;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_new_1LinkInRoute_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jint jarg2)
{
  jlong jresult            = 0;
  simgrid::s4u::Link* arg1 = (simgrid::s4u::Link*)0;
  simgrid::s4u::LinkInRoute::Direction arg2;
  simgrid::s4u::LinkInRoute* result = 0;

  (void)jenv;
  (void)jcls;
  (void)jthis;
  arg1   = *(simgrid::s4u::Link**)&cthis;
  arg2   = (simgrid::s4u::LinkInRoute::Direction)jarg2;
  result = (simgrid::s4u::LinkInRoute*)new simgrid::s4u::LinkInRoute((simgrid::s4u::Link const*)arg1, arg2);
  *(simgrid::s4u::LinkInRoute**)&jresult = result;
  return jresult;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_LinkInRoute_1get_1direction(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis)
{
  jint jresult                    = 0;
  simgrid::s4u::LinkInRoute* arg1 = (simgrid::s4u::LinkInRoute*)0;
  simgrid::s4u::LinkInRoute::Direction result;

  (void)jenv;
  (void)jcls;
  (void)jthis;
  arg1    = *(simgrid::s4u::LinkInRoute**)&cthis;
  result  = (simgrid::s4u::LinkInRoute::Direction)((simgrid::s4u::LinkInRoute const*)arg1)->get_direction();
  jresult = (jint)result;
  return jresult;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_LinkInRoute_1get_1link(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  jlong jresult                   = 0;
  simgrid::s4u::LinkInRoute* arg1 = (simgrid::s4u::LinkInRoute*)0;
  simgrid::s4u::Link* result      = 0;

  (void)jenv;
  (void)jcls;
  (void)jthis;
  arg1                            = *(simgrid::s4u::LinkInRoute**)&cthis;
  result                          = (simgrid::s4u::Link*)((simgrid::s4u::LinkInRoute const*)arg1)->get_link();
  *(simgrid::s4u::Link**)&jresult = result;
  return jresult;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1LinkInRoute(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  delete (simgrid::s4u::LinkInRoute*)cthis;
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  const char* result = ((NetZone*)cthis)->get_cname();

  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1get_1parent(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  jlong jresult                 = 0;
  simgrid::s4u::NetZone* arg1   = (simgrid::s4u::NetZone*)0;
  simgrid::s4u::NetZone* result = 0;

  (void)jenv;
  (void)jcls;
  (void)jthis;
  arg1                               = *(simgrid::s4u::NetZone**)&cthis;
  result                             = (simgrid::s4u::NetZone*)((simgrid::s4u::NetZone const*)arg1)->get_parent();
  *(simgrid::s4u::NetZone**)&jresult = result;
  return jresult;
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1get_1children(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  auto [netzone_class, netzone_ctor] = get_classctor_netzone(jenv);

  auto cnetzones = ((NetZone*)cthis)->get_children();

  jobjectArray result = jenv->NewObjectArray(cnetzones.size(), netzone_class, nullptr);
  for (unsigned i = 0; i < cnetzones.size(); i++) {
    jenv->SetObjectArrayElement(result, i, jenv->NewObject(netzone_class, netzone_ctor, cnetzones.at(i)));
    check_javaexception(jenv);
  }

  return result;
}

XBT_PUBLIC jobjectArray JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1get_1all_1hosts(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis, jobject jthis)
{
  auto [host_class, host_ctor] = get_classctor_host(jenv);

  auto chosts = ((NetZone*)cthis)->get_all_hosts();

  jobjectArray result = jenv->NewObjectArray(chosts.size(), host_class, nullptr);
  for (unsigned i = 0; i < chosts.size(); i++) {
    jenv->SetObjectArrayElement(result, i, jenv->NewObject(host_class, host_ctor, chosts.at(i)));
    check_javaexception(jenv);
  }

  return result;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1get_1host_1count(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  return ((NetZone*)cthis)->get_host_count();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1get_1property(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jstring jprop)
{
  std::string cprop  = java_string_to_std_string(jenv, jprop);
  const char* result = ((NetZone*)cthis)->get_property(cprop);
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1set_1property(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jstring jprop,
                                                                               jstring jval)
{
  if (jprop != nullptr && jval != nullptr) {
    std::string cprop = java_string_to_std_string(jenv, jprop);
    std::string cval  = java_string_to_std_string(jenv, jval);
    ((NetZone*)cthis)->set_property(cprop, cval);
  } else if (jprop == nullptr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property name shall not be null.");
  } else {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Property value shall not be null.");
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1on_1seal_1cb(JNIEnv* jenv, jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    NetZone::on_seal_cb([cb](NetZone const& nz) {
      get_jenv()->CallVoidMethod(cb, CallbackNetzone_methodId, &nz);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}
/*  NetZone_create_host__SWIG_0 (JLorg/simgrid/s4u/NetZone;Ljava/lang/String;[D)J */
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1host_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname,
                                                                                          jdoubleArray jspeeds)
{
  if (jname == nullptr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Host names shall not be null.");
    return 0;
  }
  std::string name = java_string_to_std_string(jenv, jname);

  std::vector<double> cspeeds;
  int len          = jenv->GetArrayLength(jspeeds);
  double* cjspeeds = jenv->GetDoubleArrayElements(jspeeds, nullptr);
  for (int i = 0; i < len; i++)
    cspeeds.push_back(cjspeeds[i]);
  jenv->ReleaseDoubleArrayElements(jspeeds, cjspeeds, JNI_ABORT);

  return (jlong)((NetZone*)cthis)->create_host(name, cspeeds);
}
/* NetZone_create_host__SWIG_1  (JLorg/simgrid/s4u/NetZone;Ljava/lang/String;D)J */
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1host_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname, jdouble speed)
{
  if (jname == nullptr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Host names shall not be null.");
    return 0;
  }
  std::string name = java_string_to_std_string(jenv, jname);
  return (jlong)((NetZone*)cthis)->create_host(name, speed);
}
/* NetZone_create_host__SWIG_2 (JLorg/simgrid/s4u/NetZone;Ljava/lang/String;[Ljava/lang/String;)J */
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1host_1_1SWIG_12(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname,
                                                                                          jobjectArray jspeeds)
{
  if (jname == nullptr) {
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Host names shall not be null.");
    return 0;
  }
  std::string name = java_string_to_std_string(jenv, jname);

  std::vector<std::string> speeds;
  for (int i = 0; i < jenv->GetArrayLength(jspeeds); i++)
    speeds.push_back(java_string_to_std_string(jenv, (jstring)jenv->GetObjectArrayElement(jspeeds, i)));

  return (jlong)((NetZone*)cthis)->create_host(name, speeds);
}
/* NetZone_create_host__SWIG_3 (JLorg/simgrid/s4u/NetZone;Ljava/lang/String;Ljava/lang/String;)J */
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1host_1_1SWIG_13(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname, jstring jspeed)
{
  check_nullparam(jname, "Host names shall not be null.");
  check_nullparam(jspeed, "Host speed shall not be the null string.");
  std::string name  = java_string_to_std_string(jenv, jname);
  std::string speed = java_string_to_std_string(jenv, jspeed);
  return (jlong)((NetZone*)cthis)->create_host(name, speed);
}

/* NetZone_create_link__SWIG_0   (JLorg/simgrid/s4u/NetZone;Ljava/lang/String;[D)J */
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1link_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname,
                                                                                          jdoubleArray jbandwidths)
{
  check_nullparam(jname, "Link names shall not be null.");
  check_nullparam(jbandwidths, "Link bandwidth shall not be a null array.");

  std::string name                = java_string_to_std_string(jenv, jname);
  std::vector<double> cbandwidths = java_doublearray_to_vector(jenv, jbandwidths);

  return (jlong)((NetZone*)cthis)->create_link(name, cbandwidths);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1link_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname, jdouble bw)
{
  check_nullparam(jname, "Link names shall not be null.");
  std::string name = java_string_to_std_string(jenv, jname);

  return (jlong)((NetZone*)cthis)->create_link(name, bw);
}
/* NetZone_create_link__SWIG_2   (JLorg/simgrid/s4u/NetZone;Ljava/lang/String;[Ljava/lang/String;)J  */
XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1link_1_1SWIG_12(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname,
                                                                                          jobjectArray jbws)
{
  check_nullparam(jname, "Link names shall not be null.");
  check_nullparam(jname, "Link bandwidths shall not be a null array.");
  std::string name = java_string_to_std_string(jenv, jname);
  std::vector<std::string> bws = java_stringarray_to_vector(jenv, jbws);

  return (jlong)((NetZone*)cthis)->create_host(name, bws);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1link_1_1SWIG_13(JNIEnv* jenv, jclass jcls,
                                                                                          jlong cthis, jobject jthis,
                                                                                          jstring jname, jstring jbw)
{
  check_nullparam(jname, "Link names shall not be null.");
  check_nullparam(jbw, "Link bandwidth shall not be the null string.");
  std::string name = java_string_to_std_string(jenv, jname);
  std::string bw   = java_string_to_std_string(jenv, jbw);
  return (jlong)((NetZone*)cthis)->create_link(name, bw);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1splitlink_1from_1double(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jstring jname, jdouble bwup, jdouble bwdown)
{
  check_nullparam(jname, "Link names shall not be null.");
  std::string name = java_string_to_std_string(jenv, jname);

  return (jlong)((NetZone*)cthis)->create_split_duplex_link(name, bwup, bwdown);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1create_1splitlink_1from_1string(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jstring jname, jstring jbwup, jstring jbwdown)
{
  check_nullparam(jname, "Link names shall not be null.");
  check_nullparam(jbwup, "Link up bandwidth shall not be the null string.");
  check_nullparam(jbwdown, "Link down bandwidth shall not be the null string.");
  std::string name   = java_string_to_std_string(jenv, jname);
  std::string bwup   = java_string_to_std_string(jenv, jbwup);
  std::string bwdown = java_string_to_std_string(jenv, jbwdown);
  return (jlong)((NetZone*)cthis)->create_split_duplex_link(name, bwup, bwdown);
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1add_1route_1hosts(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jlong chost1, jlong chost2,
                                                                                   jlongArray jlinks)
{
  std::vector<const Link*> clinks;
  int len       = jenv->GetArrayLength(jlinks);
  jlong* cjlinks = jenv->GetLongArrayElements(jlinks, nullptr);
  for (int i = 0; i < len; i++)
    clinks.push_back((const Link*)cjlinks[i]);
  jenv->ReleaseLongArrayElements(jlinks, cjlinks, JNI_ABORT);

  ((NetZone*)cthis)->add_route((Host*)chost1, (Host*)chost2, clinks);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1add_1route_1netzones(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis,
                                                                                      jlong cnz1, jlong cnz2,
                                                                                      jlongArray jlinks)
{
  std::vector<const Link*> clinks;
  int len       = jenv->GetArrayLength(jlinks);
  jlong* cjlinks = jenv->GetLongArrayElements(jlinks, nullptr);
  for (int i = 0; i < len; i++)
    clinks.push_back((const Link*)cjlinks[i]);
  jenv->ReleaseLongArrayElements(jlinks, cjlinks, JNI_ABORT);

  ((NetZone*)cthis)->add_route((NetZone*)cnz1, (NetZone*)cnz2, clinks);
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_NetZone_1seal(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                      jobject jthis)
{
  ((NetZone*)cthis)->seal();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1get_1name(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  try {
    const char* result = ((Mailbox*)cthis)->get_cname();
    if (result)
      return jenv->NewStringUTF(result);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1empty(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  try {
    return ((Mailbox*)cthis)->empty();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return false;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1size(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                       jobject jthis)
{
  try {
    return ((Mailbox*)cthis)->empty();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1listen(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis)
{
  bool res = false;
  try {
    res = ((Mailbox*)cthis)->listen();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return res;
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1listen_1from(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  int res = 0;
  try {
    res = ((Mailbox*)cthis)->listen_from();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return res;
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1ready(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  return ((Mailbox*)cthis)->ready();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1set_1receiver(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jlong cactor,
                                                                               jobject jactor)
{
  return ((Mailbox*)cthis)->set_receiver((Actor*)cactor);
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1get_1receiver(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis)
{
  auto result = ((Mailbox*)cthis)->get_receiver();
  return result.get() != nullptr ? result->extension<ActorJavaExt>()->jactor_ : nullptr;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1put_1init_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  CommPtr result = ((Mailbox*)cthis)->put_init();
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1put_1init_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis,
                                                                                       jobject payload,
                                                                                       jlong simulated_size_in_bytes)
{
  auto glob = jenv->NewGlobalRef(payload);

  CommPtr result = ((Mailbox*)cthis)->put_init(glob, simulated_size_in_bytes);
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1put_1async(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis, jobject payload,
                                                                             jlong simulated_size_in_bytes)
{
  auto self = (Mailbox*)cthis;

  CommPtr result = self->put_async(jenv->NewGlobalRef(payload), simulated_size_in_bytes);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1to_1c_1str(JNIEnv* jenv, jclass jcls, jint cthis)
{
  auto arg1          = (simgrid::s4u::Mailbox::IprobeKind)cthis;
  const char* result = simgrid::s4u::Mailbox::to_c_str(arg1);
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1get_1init(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis)
{
  auto self = (Mailbox*)cthis;

  CommPtr result = self->get_init();
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1get_1async(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis)
{
  auto self = (Mailbox*)cthis;

  CommPtr result = self->get_async();
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1clear(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                       jobject jthis)
{
  ((Mailbox*)cthis)->clear();
}
XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1put_1_1SWIG_10(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jobject payload,
                                                                                jlong simulated_size_in_bytes)
{
  auto self = (Mailbox*)cthis;
  try {
    self->put(jenv->NewGlobalRef(payload), (uint64_t)simulated_size_in_bytes);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1put_1_1SWIG_11(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jobject payload,
                                                                                jlong simulated_size_in_bytes,
                                                                                jdouble timeout)
{
  auto self = (Mailbox*)cthis;
  try {
    self->put(jenv->NewGlobalRef(payload), simulated_size_in_bytes, timeout);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Mailbox_1get(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                        jobject jthis)
{
  try {
    auto self    = (Mailbox*)cthis;
    jobject glob = self->get<_jobject>();
    auto local   = jenv->NewLocalRef(glob);
    jenv->DeleteGlobalRef(glob);

    return local;

  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1set_1queue(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                        jobject jthis, jlong cqueue, jobject jqueue)
{
  ((Mess*)cthis)->set_queue((MessageQueue*)cqueue);
}

JNIEXPORT jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1get_1queue(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                         jobject jthis)
{
  return (jlong)((Mess*)cthis)->get_queue();
}

JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1set_1payload(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                          jobject jthis, jobject payload)
{
  auto self = (Mess*)cthis;
  auto glob = jenv->NewGlobalRef(payload);

  self->set_payload(glob);
}

JNIEXPORT jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1get_1payload(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                             jobject jthis)
{
  auto self    = (Mess*)cthis;
  jobject glob = (jobject)self->get_payload();
  auto local   = jenv->NewLocalRef(glob);
  jenv->DeleteGlobalRef(glob);

  return local;
}

JNIEXPORT jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1get_1sender(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                            jobject jthis)
{
  ActorPtr result = ((Mess*)cthis)->get_sender();
  return result.get() != nullptr ? result->extension<ActorJavaExt>()->jactor_ : nullptr;
}

JNIEXPORT jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1is_1assigned(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                              jobject jthis)
{
  return ((Mess*)cthis)->is_assigned();
}

JNIEXPORT jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1get_1receiver(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                              jobject jthis)
{
  ActorPtr result = ((Mess*)cthis)->get_receiver();
  return result.get() != nullptr ? result->extension<ActorJavaExt>()->jactor_ : nullptr;
}

JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1add_1successor(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                            jobject jthis, jlong cact, jobject jact)
{
  auto self = (Mess*)cthis;
  self->add_successor((Activity*)cact);
}

JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1remove_1successor(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                               jobject jthis, jlong cact, jobject jact)
{
  auto self = (Mess*)cthis;
  self->remove_successor((Activity*)cact);
}

JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1set_1name(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                       jobject jthis, jstring jname)
{
  auto* self = (Mess*)cthis;
  self->set_name(java_string_to_std_string(jenv, jname));
}

JNIEXPORT jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1get_1name(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                          jobject jthis)
{
  const char* result = ((Mess*)cthis)->get_cname();
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1set_1tracing_1category(JNIEnv* jenv, jclass klass,
                                                                                    jlong cthis, jobject jthis,
                                                                                    jstring jcat)
{
  auto self = (Mess*)cthis;
  self->set_tracing_category(java_string_to_std_string(jenv, jcat));
}

JNIEXPORT jstring JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1get_1tracing_1category(JNIEnv* jenv, jclass klass,
                                                                                       jlong cthis, jobject jthis)
{
  std::string result = ((Mess*)cthis)->get_tracing_category();
  return jenv->NewStringUTF(result.c_str());
}

JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1cancel(JNIEnv* jenv, jclass klass, jlong cthis,
                                                                    jobject jthis)
{
  ((Mess*)cthis)->cancel();
}

XBT_PUBLIC JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_Mess_1await_1for(JNIEnv* jenv, jclass klass,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jdouble jarg2)
{
  try {
    ((Mess*)cthis)->wait_for(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1get_1name(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  const char* result = ((MessageQueue*)cthis)->get_cname();

  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1empty(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  return ((MessageQueue*)cthis)->empty();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1size(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                            jobject jthis)
{
  return ((MessageQueue*)cthis)->size();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1put_1init_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                            jlong cthis, jobject jthis)
{
  auto self = (MessageQueue*)cthis;

  MessPtr result = self->put_init();
  intrusive_ptr_add_ref(result.get());

  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1put_1init_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                            jlong cthis, jobject jthis,
                                                                                            jobject payload)
{
  auto self = (MessageQueue*)cthis;

  MessPtr result = self->put_init(jenv->NewGlobalRef(payload));
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1put_1async(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis,
                                                                                  jobject payload)
{
  auto self = (MessageQueue*)cthis;

  MessPtr result = self->put_async(jenv->NewGlobalRef(payload));
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1put_1_1SWIG_10(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jobject payload)
{
  auto self = (MessageQueue*)cthis;
  try {
    self->put(jenv->NewGlobalRef(payload));
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1put_1_1SWIG_11(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jobject payload, jdouble jarg3)
{
  try {
    ((simgrid::s4u::MessageQueue*)cthis)->put(jenv->NewGlobalRef(payload), jarg3);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1get_1init(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  try {
    auto self = (MessageQueue*)cthis;

    MessPtr result = self->get_init();
    intrusive_ptr_add_ref(result.get());
    return (jlong)result.get();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_MessageQueue_1get_1async(JNIEnv* jenv, jclass jcls,
                                                                                  jlong cthis, jobject jthis)
{
  try {
    auto self = (MessageQueue*)cthis;

    MessPtr result = self->get_async();
    intrusive_ptr_add_ref(result.get());
    return (jlong)result.get();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  } catch (std::exception const& e) {
    rethrow_simgrid_exception(jenv, e);
  }
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1Mutex(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  intrusive_ptr_release((Mutex*)cthis);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Mutex_1create(JNIEnv* jenv, jclass jcls, jboolean recursive)
{
  MutexPtr result = simgrid::s4u::Mutex::create(recursive);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Mutex_1lock(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                    jobject jthis)
{
  try {
    ((Mutex*)cthis)->lock();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Mutex_1unlock(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                      jobject jthis)
{
  ((Mutex*)cthis)->unlock();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Mutex_1try_1lock(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis)
{
  return ((Mutex*)cthis)->try_lock();
}

XBT_PUBLIC jobject JNICALL Java_org_simgrid_s4u_simgridJNI_Mutex_1get_1owner(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                             jobject jthis)
{
  ActorPtr result = ((Mutex*)cthis)->get_owner();
  return result.get() != nullptr ? result->extension<ActorJavaExt>()->jactor_ : nullptr;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_delete_1Semaphore(JNIEnv* jenv, jclass jcls, jlong cthis)
{
  intrusive_ptr_release((Semaphore*)cthis);
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_Semaphore_1create(JNIEnv* jenv, jclass jcls, jlong capa)
{
  SemaphorePtr result = simgrid::s4u::Semaphore::create(capa);
  intrusive_ptr_add_ref(result.get());
  return (jlong)result.get();
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Semaphore_1acquire(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  try {
    ((Semaphore*)cthis)->acquire();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Semaphore_1acquire_1timeout(JNIEnv* jenv, jclass jcls,
                                                                                        jlong cthis, jobject jthis,
                                                                                        jdouble jarg2)
{
  bool res = false;
  try {
    res = ((Semaphore*)cthis)->acquire_timeout(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
  return res;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_Semaphore_1release(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                           jobject jthis)
{
  ((Semaphore*)cthis)->release();
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_Semaphore_1get_1capacity(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  return ((Semaphore*)cthis)->get_capacity();
}

XBT_PUBLIC jboolean JNICALL Java_org_simgrid_s4u_simgridJNI_Semaphore_1would_1block(JNIEnv* jenv, jclass jcls,
                                                                                    jlong cthis, jobject jthis)
{
  return ((Semaphore*)cthis)->would_block();
}

XBT_PUBLIC jstring JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1to_1c_1str(JNIEnv* jenv, jclass jcls,
                                                                                      jint cthis)
{
  auto arg1          = (simgrid::s4u::VirtualMachine::State)cthis;
  const char* result = VirtualMachine::to_c_str(arg1);
  if (result)
    return jenv->NewStringUTF(result);
  return 0;
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1start(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                              jobject jthis)
{
  try {
    ((VirtualMachine*)cthis)->start();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1suspend(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  try {
    ((VirtualMachine*)cthis)->suspend();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1resume(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis)
{
  try {
    ((VirtualMachine*)cthis)->resume();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1shutdown(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  try {
    ((VirtualMachine*)cthis)->start();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1destroy(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis)
{
  try {
    ((VirtualMachine*)cthis)->destroy();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1get_1pm(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                 jobject jthis)
{
  try {
    return (jlong)((VirtualMachine*)cthis)->get_pm();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
    return 0;
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1set_1pm(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                                jobject jthis, jlong chost,
                                                                                jobject jarg2_)
{
  try {
    ((VirtualMachine*)cthis)->set_pm((Host*)chost);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jlong JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1get_1ramsize(JNIEnv* jenv, jclass jcls,
                                                                                      jlong cthis, jobject jthis)
{
  try {
    return ((VirtualMachine*)cthis)->get_ramsize();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
    return 0;
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1set_1ramsize(JNIEnv* jenv, jclass jcls,
                                                                                     jlong cthis, jobject jthis,
                                                                                     jlong jarg2)
{
  try {
    ((VirtualMachine*)cthis)->set_ramsize(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1set_1bound(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis,
                                                                                   jdouble jarg2)
{
  try {
    ((VirtualMachine*)cthis)->set_bound(jarg2);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}
JNIEXPORT void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1migrate(JNIEnv* jenv, jclass jcls, jlong cthis,
                                                                               jobject jthis, jlong chost,
                                                                               jobject jhost)
{
  try {
    sg_vm_migrate((VirtualMachine*)cthis, (Host*)chost);
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1start_1migration(JNIEnv* jenv, jclass jcls,
                                                                                         jlong cthis, jobject jthis)
{
  try {
    ((VirtualMachine*)cthis)->start_migration();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1end_1migration(JNIEnv* jenv, jclass jcls,
                                                                                       jlong cthis, jobject jthis)
{
  try {
    ((VirtualMachine*)cthis)->end_migration();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
  }
}

XBT_PUBLIC jint JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1get_1state(JNIEnv* jenv, jclass jcls,
                                                                                   jlong cthis, jobject jthis)
{
  try {
    return (jint)((VirtualMachine*)cthis)->get_state();
  } catch (ForcefulKillException const&) { /* Actor killed, this is fine. */
    return 0;
  }
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1start_1cb(JNIEnv* jenv, jclass jcls,
                                                                                      jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    VirtualMachine::on_start_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1start_1cb(JNIEnv* jenv, jclass jcls,
                                                                                            jlong cthis, jobject jthis,
                                                                                            jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_start_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1started_1cb(JNIEnv* jenv, jclass jcls,
                                                                                        jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    VirtualMachine::on_started_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1started_1cb(JNIEnv* jenv, jclass jcls,
                                                                                              jlong cthis,
                                                                                              jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_this_started_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1shutdown_1cb(JNIEnv* jenv, jclass jcls,
                                                                                         jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    VirtualMachine::on_shutdown_cb(
        [cb](VirtualMachine const& vm) { get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm); });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1shutdown_1cb(JNIEnv* jenv,
                                                                                               jclass jcls, jlong cthis,
                                                                                               jobject jthis,
                                                                                               jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_this_shutdown_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1suspend_1cb(JNIEnv* jenv, jclass jcls,
                                                                                              jlong cthis,
                                                                                              jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_this_suspend_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1resume_1cb(JNIEnv* jenv, jclass jcls,
                                                                                             jlong cthis, jobject jthis,
                                                                                             jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_this_resume_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1destruction_1cb(JNIEnv* jenv, jclass jcls,
                                                                                            jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    VirtualMachine::on_destruction_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1destruction_1cb(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_shutdown_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1migration_1start_1cb(JNIEnv* jenv,
                                                                                                 jclass jcls,
                                                                                                 jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    VirtualMachine::on_migration_start_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1migration_1start_1cb(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_this_migration_start_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1migration_1end_1cb(JNIEnv* jenv,
                                                                                               jclass jcls, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    VirtualMachine::on_migration_end_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

XBT_PUBLIC void JNICALL Java_org_simgrid_s4u_simgridJNI_VirtualMachine_1on_1this_1migration_1end_1cb(
    JNIEnv* jenv, jclass jcls, jlong cthis, jobject jthis, jobject cb)
{
  if (cb) {
    cb = jenv->NewGlobalRef(cb);
    ((VirtualMachine*)cthis)->on_this_migration_end_cb([cb](VirtualMachine const& vm) {
      get_jenv()->CallVoidMethod(cb, CallbackDisk_methodId, &vm);
      exception_check_after_upcall(get_jenv());
    });
  } else
    SWIG_JavaThrowException(jenv, SWIG_JavaNullPointerException, "Callbacks shall not be null.");
}

#ifdef __cplusplus
}
#endif
