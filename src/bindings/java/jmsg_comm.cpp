/* Functions related to the java comm instances                             */

/* Copyright (c) 2012-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "jmsg_comm.h"
#include "jxbt_utilities.h"
#include "jmsg.h"

#include <simgrid/msg.h>
XBT_LOG_EXTERNAL_DEFAULT_CATEGORY(jmsg);

static jfieldID jcomm_field_Comm_bind;
static jfieldID jcomm_field_Comm_finished;
static jfieldID jcomm_field_Comm_receiving;
static jfieldID jtask_field_Comm_task;
static jfieldID jcomm_field_Comm_taskBind;

void jcomm_bind_task(JNIEnv *env, jobject jcomm) {
  msg_comm_t comm = (msg_comm_t) (uintptr_t) env->GetLongField(jcomm, jcomm_field_Comm_bind);
  //test if we are receiving or sending a task.
  jboolean jreceiving = env->GetBooleanField(jcomm, jcomm_field_Comm_receiving);
  if (jreceiving == JNI_TRUE) {
    //bind the task object.
    msg_task_t task = MSG_comm_get_task(comm);
    xbt_assert(task != nullptr, "Task is nullptr");
    jobject jtask_global = static_cast<jobject>(MSG_task_get_data(task));
    //case where the data has already been retrieved
    if (jtask_global == nullptr) {
      return;
    }

    //Make sure the data will be correctly gc.
    jobject jtask_local = env->NewLocalRef(jtask_global);
    env->DeleteGlobalRef(jtask_global);

    env->SetObjectField(jcomm, jtask_field_Comm_task, jtask_local);

    MSG_task_set_data(task, nullptr);
  }
}

JNIEXPORT void JNICALL Java_org_simgrid_msg_Comm_nativeInit(JNIEnv *env, jclass cls) {
  jclass jfield_class_Comm = env->FindClass("org/simgrid/msg/Comm");
  if (!jfield_class_Comm) {
    jxbt_throw_native(env,bprintf("Can't find the org/simgrid/msg/Comm class."));
    return;
  }
  jcomm_field_Comm_bind = jxbt_get_jfield(env, jfield_class_Comm, "bind", "J");
  jcomm_field_Comm_taskBind  = jxbt_get_jfield(env, jfield_class_Comm, "taskBind", "J");
  jcomm_field_Comm_receiving = jxbt_get_jfield(env, jfield_class_Comm, "receiving", "Z");
  jtask_field_Comm_task = jxbt_get_jfield(env, jfield_class_Comm, "task", "Lorg/simgrid/msg/Task;");
  jcomm_field_Comm_finished = jxbt_get_jfield(env, jfield_class_Comm, "finished", "Z");
  if (!jcomm_field_Comm_bind || !jcomm_field_Comm_taskBind || !jcomm_field_Comm_receiving || !jtask_field_Comm_task ||
      !jcomm_field_Comm_finished) {
    jxbt_throw_native(env,bprintf("Can't find some fields in Java class."));
  }
}

JNIEXPORT void JNICALL Java_org_simgrid_msg_Comm_nativeFinalize(JNIEnv *env, jobject jcomm) {
  msg_comm_t comm;
  msg_task_t *task_received;

  task_received = (msg_task_t*)  (uintptr_t) env->GetLongField(jcomm, jcomm_field_Comm_taskBind);
  xbt_free(task_received);

  comm = (msg_comm_t) (uintptr_t) env->GetLongField(jcomm, jcomm_field_Comm_bind);
  MSG_comm_destroy(comm);
}

JNIEXPORT jboolean JNICALL Java_org_simgrid_msg_Comm_test(JNIEnv *env, jobject jcomm) {
  msg_comm_t comm;
  comm = (msg_comm_t) (uintptr_t) env->GetLongField(jcomm, jcomm_field_Comm_bind);

  jboolean finished = env->GetBooleanField(jcomm, jcomm_field_Comm_finished);
  if (finished == JNI_TRUE) {
    return JNI_TRUE;
  }

  if (!comm) {
    jxbt_throw_native(env,bprintf("comm is null"));
    return JNI_FALSE;
  }

  if (MSG_comm_test(comm)) {
    msg_error_t status = MSG_comm_get_status(comm);
    if (status == MSG_OK) {
      jcomm_bind_task(env,jcomm);
      return JNI_TRUE;
    } else {
      //send the correct exception
      jmsg_throw_status(env,status);
    }
  }
  return JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_simgrid_msg_Comm_waitCompletion(JNIEnv *env, jobject jcomm, jdouble timeout) {
  msg_comm_t comm = (msg_comm_t) (uintptr_t) env->GetLongField(jcomm, jcomm_field_Comm_bind);
  if (!comm) {
    jxbt_throw_native(env,bprintf("comm is null"));
    return;
  }

  jboolean finished = env->GetBooleanField(jcomm, jcomm_field_Comm_finished);
  if (finished == JNI_TRUE) {
    return;
  }

  msg_error_t status;
  status = MSG_comm_wait(comm,static_cast<double>(timeout));
  env->SetBooleanField(jcomm, jcomm_field_Comm_finished, JNI_TRUE);
  if (status == MSG_OK) {
    jcomm_bind_task(env,jcomm);
    return;
  } else {
    jmsg_throw_status(env,status);
  }
}

static msg_comm_t* jarray_to_commArray(JNIEnv *env, jobjectArray jcomms, /* OUT */ int *count)
{
  *count = env->GetArrayLength(jcomms);
  msg_comm_t* comms = xbt_new(msg_comm_t, *count);

  for (int i=0; i < *count; i++) {
     jobject jcomm = env->GetObjectArrayElement(jcomms, i);
     if (env->ExceptionOccurred())
        break;

     comms[i] = (msg_comm_t) (uintptr_t) env->GetLongField(jcomm, jcomm_field_Comm_bind);
     if (!comms[i]) {
       jxbt_throw_native(env,bprintf("comm at rank %d is null",i));
       return nullptr;
     }

     env->DeleteLocalRef(jcomm); // reduce the load on the garbage collector: we don't need that object anymore
  }
  return comms;
}
JNIEXPORT void JNICALL Java_org_simgrid_msg_Comm_waitAll(JNIEnv *env, jclass cls, jobjectArray jcomms, jdouble timeout)
{
  int count;
  msg_comm_t* comms = jarray_to_commArray(env, jcomms, &count);
  if (!comms)
    return;

  MSG_comm_waitall(comms, count, static_cast<double>(timeout));
  xbt_free(comms);
}
JNIEXPORT int JNICALL Java_org_simgrid_msg_Comm_waitAny(JNIEnv *env, jclass cls, jobjectArray jcomms)
{
  int count;
  msg_comm_t* comms = jarray_to_commArray(env, jcomms, &count);
  if (!comms)
    return -1;
  xbt_dynar_t dyn = xbt_dynar_new(sizeof(msg_comm_t),nullptr);
  for (int i=0; i<count; i++) {
    xbt_dynar_push(dyn, &(comms[i]));
  }

  int rank = MSG_comm_waitany(dyn);
  xbt_free(comms);
  xbt_dynar_free(&dyn);
  return rank;
}
