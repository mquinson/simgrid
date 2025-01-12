if(NOT SIMGRID_HAVE_MC)
  foreach (example mc-failing-assert mc-centralized-mutex)
    set(_${example}_disable 1)
  endforeach()
endif()

foreach(example actor-create actor-daemon actor-join actor-kill actor-migrate actor-suspend actor-yield actor-lifetime
        activityset-testany activityset-waitall activityset-waitallfor activityset-waitany
        app-masterworkers
        comm-wait comm-failure comm-host2host comm-pingpong
        comm-ready comm-suspend comm-throttling comm-waituntil
        exec-async exec-basic exec-dvfs exec-remote exec-ptask
        task-io task-simple task-switch-host task-variable-load
        platform-comm-serialize platform-profile platform-failures
        plugin-host-load
        mc-failing-assert mc-centralized-mutex
        network-nonlinear clusters-multicpu io-degradation exec-cpu-nonlinear
        synchro-barrier synchro-mutex synchro-semaphore
        smpi_all2all smpi_allreduce smpi_allgather smpi_bcast smpi_reduce smpi_reducescatter)
  set(tesh_files    ${tesh_files}   ${CMAKE_CURRENT_SOURCE_DIR}/${example}/${example}.tesh)
  set(examples_src  ${examples_src} ${CMAKE_CURRENT_SOURCE_DIR}/${example}/${example}.py)

  if(enable_python)
    if(NOT DEFINED _${example}_disable)
      ADD_TESH(python-${example}
                               --setenv bindir=${CMAKE_BINARY_DIR}
                               --setenv srcdir=${CMAKE_CURRENT_SOURCE_DIR}/${example}
                               --setenv platfdir=${CMAKE_HOME_DIRECTORY}/examples/platforms
                               --setenv pythoncmd=${PYTHON_EXECUTABLE}
                               --setenv LD_LIBRARY_PATH=${TESH_LIBRARY_PATH}
                               --setenv PYTHONPATH=${CMAKE_BINARY_DIR}/lib
                               --cd ${CMAKE_CURRENT_SOURCE_DIR}/${example}
                               ${example}.tesh)
    else()
      message(STATUS "Example python-${example} disabled, thus not compiled.")
      unset(_${example}_disable)
    endif()
  endif()
endforeach()

set(examples_src  ${examples_src}                                                      PARENT_SCOPE)
set(tesh_files    ${tesh_files}   examples/python/actor-create/actor-create_d.xml
                                  examples/python/actor-lifetime/actor-lifetime_d.xml
                                  examples/python/app-masterworkers/app-masterworkers_d.xml
                                  examples/python/platform-failures/platform-failures_d.xml
                                  PARENT_SCOPE)
