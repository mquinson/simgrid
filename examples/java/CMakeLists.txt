if(SIMGRID_HAVE_JAVA)
  add_custom_target(java-all COMMENT "Building all Java examples...")
  add_dependencies(tests java-all)
  add_dependencies(java-all simgrid_jar)
  add_dependencies(java-all simgrid-java) # useful when the libs are not included in the jar
  add_dependencies(java-all simgrid)      # useful when the libs are not included in the jar

  if(${Java_VERSION} VERSION_GREATER_EQUAL "24.0.0")
    set(javac_args "--enable-native-access=ALL-UNNAMED")
  else()
    message(STATUS "Java version is ${Java_VERSION}, no need to specifically enable the access to native libs.")
  endif()
endif()

if(NOT HAVE_GRAPHVIZ)
  set(_dag_from_dot_disable 1)
  set(_dag_from_dot_simple_disable 1)
endif()
if (NOT SIMGRID_HAVE_JSON)
  set(_dag_from_json_simple_disable 1)
endif()

# io_dependent io_file_system io_file_remote: need a plugin
# exec_failure: needs state profiles
foreach (example activityset_awaitall activityset_awaitallfor activityset_awaitany activityset_testany
                 actor_create actor_daemon actor_exiting actor_join actor_kill
                 actor_lifetime actor_migrate actor_suspend actor_yield
                 cloud_migration cloud_simple #cloud_capping
                 dag_comm dag_failure dag_from_dax_simple dag_from_dax dag_from_dot_simple dag_from_dot dag_from_json_simple dag_io dag_scheduling dag_simple dag_tuto
                 energy_boot energy_exec energy_exec_ptask energy_link energy_vm energy_wifi
                 exec_async exec_awaitfor exec_basic exec_cpu_factors exec_dependent exec_dvfs exec_threads exec_ptask exec_ptask_multicore exec_ptask_multicore_latency exec_remote exec_suspend exec_unassigned
#                 exec_cpu_nonlinear
                 comm_await comm_awaituntil comm_dependent comm_failure comm_host2host comm_pingpong comm_ready comm_suspend comm_throttling
                 io_async io_disk_raw io_priority # io_degradation 
                 mess_await
                 platform_comm_serialize # platform_failures  platform_profile  platform_properties
                 synchro_barrier synchro_condition_variable synchro_condition_variable_awaituntil synchro_mutex synchro_semaphore
                 #task_dispatch  task_io  task_microservice  task_parallelism  task_simple  task_storm  task_switch_host  task_variable_load
                 )
  
  # Use default source file unless specified otherwise
  if(NOT DEFINED _${example}_sources)
    set(_${example}_sources ${CMAKE_CURRENT_SOURCE_DIR}/${example}/${example}.java)
  endif()
  # Use default tesh file unless specified otherwise
  if(NOT DEFINED _${example}_teshfile)
    set(_${example}_teshfile ${CMAKE_CURRENT_SOURCE_DIR}/${example}/${example}.java.tesh)
  endif()

  if(SIMGRID_HAVE_JAVA AND (NOT DEFINED _${example}_disable))
    add_jar(java-${example} SOURCES ${_${example}_sources}
            INCLUDE_JARS simgrid_jar
            ENTRY_POINT ${example}
            OUTPUT_NAME ${example}
            OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${example_dir})
    add_dependencies(java-all java-${example})
    ADD_TESH(java-${example} --setenv LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}:${CMAKE_BINARY_DIR}/lib --setenv LC_CTYPE=C --setenv javacmd="${Java_JAVA_EXECUTABLE} -Xcheck:jni ${javac_args}" --setenv platfdir=${CMAKE_HOME_DIRECTORY}/examples/platforms --setenv srcdir=${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/${example}/${example}.java.tesh)
  endif()

  set(examples_src  ${examples_src}  ${_${example}_sources})
  set(tesh_files    ${tesh_files}    ${CMAKE_CURRENT_SOURCE_DIR}/${example}/${example}.java.tesh)
endforeach()

set(examples_src  ${examples_src}                                                                          PARENT_SCOPE)
set(tesh_files    ${tesh_files}                                                                            PARENT_SCOPE)
set(xml_files     ${xml_files}    ${CMAKE_CURRENT_SOURCE_DIR}/actor_lifetime/actor_lifetime_d.xml
                                  ${CMAKE_CURRENT_SOURCE_DIR}/actor_create/actor_create_d.xml
                                  ${CMAKE_CURRENT_SOURCE_DIR}/dag_scheduling/Montage_25.xml
                  PARENT_SCOPE)
set(txt_files     ${txt_files}    ${CMAKE_CURRENT_SOURCE_DIR}/dag_from_dax/simple_dax_with_cycle.xml
                                  ${CMAKE_CURRENT_SOURCE_DIR}/dag_from_dax/smalldax.xml
                                  ${CMAKE_CURRENT_SOURCE_DIR}/dag_from_dax_simple/dag.xml
                                  ${CMAKE_CURRENT_SOURCE_DIR}/dag_from_dot/dag.dot
                                  ${CMAKE_CURRENT_SOURCE_DIR}/dag_from_dot/dag_with_cycle.dot
                                  ${CMAKE_CURRENT_SOURCE_DIR}/dag_from_dot_simple/dag.dot
                                  ${CMAKE_CURRENT_SOURCE_DIR}/dag_from_json_simple/dag.json
                                  #${CMAKE_CURRENT_SOURCE_DIR}/replay_comm/s4u-replay-comm-split-p0.txt
                                  #${CMAKE_CURRENT_SOURCE_DIR}/replay_comm/s4u-replay-comm-split-p1.txt
                                  #${CMAKE_CURRENT_SOURCE_DIR}/replay_comm/s4u-replay-comm.txt
                                  #${CMAKE_CURRENT_SOURCE_DIR}/replay_io/s4u-replay-io.txt
                    PARENT_SCOPE)
