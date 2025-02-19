# Build the version number

set(SIMGRID_VERSION_MAJOR "3")
set(SIMGRID_VERSION_MINOR "36")
set(SIMGRID_VERSION_PATCH "1") # odd => git branch; even => stable release or released snapshot

if(${SIMGRID_VERSION_PATCH} EQUAL "0")
  set(release_version "${SIMGRID_VERSION_MAJOR}.${SIMGRID_VERSION_MINOR}")
else()
  set(release_version "${SIMGRID_VERSION_MAJOR}.${SIMGRID_VERSION_MINOR}.${SIMGRID_VERSION_PATCH}")
endif()

if(WIN32 OR MINGW)
  message(FATAL_ERROR "SimGrid does not build on native windows, nor with MinGW. Please use WSL2 instead.")
endif()

message(STATUS "Configuring SimGrid v${release_version}")

set(SIMGRID_VERSION_STRING "SimGrid version ${release_version}")

set(libsimgrid_version "${release_version}")
set(libsimgrid-java_version "${release_version}")

# Basic checks on cmake
cmake_minimum_required(VERSION 3.12)
# once we move CMake to >= 3.13, we should use target_link_option in examples/sthread
# once we move CMake to >= 3.13.1, we could get rid of _Boost_STACKTRACE_BACKTRACE_HEADERS
# When updating, try to remove some cmake_policy() to see if it's OK or if it generates a warning
message(STATUS "Cmake version ${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}.${CMAKE_PATCH_VERSION}")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_HOME_DIRECTORY}/tools/cmake/Modules)

project(simgrid C CXX)

# customizable installation directories
include(GNUInstallDirs)

#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#
#     Check for the compiler        #
#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#-#

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
## Request full debugging flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g3")

if (CMAKE_COMPILER_IS_GNUCC)
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "7.0")
    message(FATAL_ERROR
            "SimGrid needs at least g++ version 7.0 to compile but you have ${CMAKE_CXX_COMPILER_VERSION}."
            "You need a sufficient support of c++17 to compile SimGrid.")
  endif()
endif()

## We need a decent support of the C++17 and C11 standards
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (CMAKE_C_COMPILER_ID STREQUAL "Intel" AND CMAKE_C11_EXTENSION_COMPILE_OPTION STREQUAL "-std=c11")
  set(CMAKE_C11_EXTENSION_COMPILE_OPTION "-std=gnu11")
endif()

### Check threading support
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
find_package(Threads)

### Setup Options
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/Option.cmake)

### SMPI vs. Fortran
if ((NOT DEFINED enable_smpi) OR enable_smpi)
  # First unset the compiler in case we're re-running cmake over a previous
  # configuration where it was saved as smpiff
  unset(CMAKE_Fortran_COMPILER)

  SET(SMPI_FORTRAN OFF)
  if(enable_fortran)
    enable_language(Fortran OPTIONAL)
  endif()

  if(CMAKE_Fortran_COMPILER)

    # Fortran compiler detected: save it, then replace by smpiff
    set(SMPI_Fortran_COMPILER "${CMAKE_Fortran_COMPILER}" CACHE FILEPATH "The real Fortran compiler")

	# Set flags/libs to be used in smpiff
    if(CMAKE_Fortran_COMPILER_ID MATCHES "GNU")
      set(SMPI_Fortran_FLAGS_ "\"-fpic\" \"-ff2c\" \"-fno-second-underscore\"")
      set(SMPI_Fortran_LIBS "\"-lgfortran\"")
      set(SMPI_GFORTRAN 1)
    elseif(CMAKE_Fortran_COMPILER_ID MATCHES "Intel")
      set(SMPI_Fortran_FLAGS_ "\"-fPIC\" \"-nofor-main\"")
      set(SMPI_Fortran_LIBS "\"-lifcore\"")
      set(SMPI_IFORT 1)
    elseif(CMAKE_Fortran_COMPILER_ID MATCHES "PGI|Flang") # flang
      set(SMPI_Fortran_FLAGS_ "\"-fPIC\"")
      set(SMPI_Fortran_LIBS "")
      set(SMPI_FLANG 1)
    endif()
    set(SMPI_Fortran_FLAGS "${SMPI_Fortran_FLAGS_} ${SMPI_Fortran_FLAGS}")

    ## Request debugging flags for Fortran too
    set(CMAKE_Fortran_FLAGS "${CMAKE_Fortran_FLAGS} -g")

    set(SMPI_FORTRAN ON)
  endif(CMAKE_Fortran_COMPILER)

endif()

### SET THE LIBRARY EXTENSION
if(APPLE)
  set(LIB_EXE "dylib")
else()
  set(LIB_EXE "so")
endif()

execute_process(COMMAND ${CMAKE_LINKER} -v OUTPUT_VARIABLE LINKER_VERSION ERROR_VARIABLE LINKER_VERSION)
string(REGEX MATCH "[0-9].[0-9]*" LINKER_VERSION "${LINKER_VERSION}")

### Find programs and paths
FIND_PROGRAM(GCOV_PATH gcov)
include(FindPerl)
if(NOT PERL_FOUND)
  message(FATAL_ERROR "Please install Perl to compile SimGrid.")
endif()

# tesh.py needs python 3
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
  message(FATAL_ERROR "Please install Python (version 3 or higher) to compile SimGrid.")
endif()
set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})

SET(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/lib)

### Compute the include paths

# Only include public headers by default
include_directories(
   ${CMAKE_BINARY_DIR}/include
   ${CMAKE_HOME_DIRECTORY}/include
)

# Compute the ones that should be added when compiling the library
set(INTERNAL_INCLUDES
  ${CMAKE_BINARY_DIR}
  ${CMAKE_HOME_DIRECTORY}
  )

if(enable_smpi)
  set (INTERNAL_INCLUDES ${INTERNAL_INCLUDES} ${CMAKE_HOME_DIRECTORY}/src/smpi/include)
endif()

if(NOT CMAKE_CROSSCOMPILING AND EXISTS /usr/include/)
  set(INTERNAL_INCLUDES ${INTERNAL_INCLUDES} /usr/include/)
endif()

# library dependency cannot start with a space (CMP0004), so initialize it with something that is never deactivated.
set(SIMGRID_DEP "-lm")

### Determine the assembly flavor that we need today
set(HAVE_RAW_CONTEXTS 0)
include(CMakeDetermineSystem)
foreach(arch i686 x86_64 arm64)
  set(SIMGRID_PROCESSOR_${arch} 0)
endforeach()
IF(CMAKE_SYSTEM_PROCESSOR MATCHES ".86|AMD64|amd64")
  IF(CMAKE_SIZEOF_VOID_P EQUAL 4) # 32 bits
    message(STATUS "System processor: i686 (${CMAKE_SYSTEM_PROCESSOR}, 32 bits)")
    set(SIMGRID_PROCESSOR_i686 1)
  ELSE()
    message(STATUS "System processor: x86_64 (${CMAKE_SYSTEM_PROCESSOR}, 64 bits)")
    set(SIMGRID_PROCESSOR_x86_64 1)
  ENDIF()
  if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    message(STATUS "Disable fast raw contexts on x32 ABI.")
  else()
    set(HAVE_RAW_CONTEXTS 1)
  endif()
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  message(STATUS "System processor: arm64 (${CMAKE_SYSTEM_PROCESSOR}, 64 bits)")
  set(SIMGRID_PROCESSOR_arm64 1)
ELSE()
  message(STATUS "System processor (${CMAKE_SYSTEM_PROCESSOR}) not explicitly accounted for")
ENDIF()

### Determine wether we use the MUSL libc, as some advanced privatization features fail with it
set(SIMGRID_HAVE_MUSL OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  try_run(run_musl compile_musl ${CMAKE_BINARY_DIR} ${CMAKE_HOME_DIRECTORY}/tools/cmake/test_prog/prog_musl.c
    COMPILE_OUTPUT_VARIABLE compile_musl_output)

  #If can have both context
  if(NOT compile_musl)
    message(FATAL_ERROR "Error: Failed to detect whether we use the MUSL libc.\n${compile_musl_output}")
  elseif(run_musl)
    message(STATUS "MUSL libc detected.")
    set(SIMGRID_HAVE_MUSL ON)
  else()
    message(STATUS "GNU libc detected.")
  endif()
endif()

include(CheckFunctionExists)
include(CheckTypeSize)
include(CheckIncludeFile)
include(CheckIncludeFiles)
include(CheckLibraryExists)
include(CheckSymbolExists)

set(HAVE_GRAPHVIZ OFF)
if(minimal-bindings)
  message(STATUS "Don't even look for graphviz, as we build minimal binding libraries.")
else()
  include(FindGraphviz)
endif()

set(SIMGRID_HAVE_NS3 OFF)
if(enable_ns3)
  include(FindNS3)
  if (SIMGRID_HAVE_NS3)
    if (NOT NS3_VERSION EQUAL "3-dev" AND NS3_VERSION VERSION_LESS "3.28")
      message(FATAL_ERROR "SimGrid needs ns-3 in version 3.28 or higher. Please upgrade or disable that cmake option.")
    endif()
    if ((NOT NS3_VERSION EQUAL "3-dev") AND NS3_VERSION VERSION_GREATER "3.41")
      # ns3 v3.42 needs C++20 because timer-impl.h uses std::remove_cvref_t
      message(STATUS "Activating C++20 for NS3 ${NS3_VERSION}")
      set_source_files_properties(src/kernel/resource/models/network_ns3.cpp
                                  PROPERTIES COMPILE_FLAGS -std=gnu++20)
      set_source_files_properties(src/kernel/resource/models/ns3/ns3_simulator.cpp
                                  PROPERTIES COMPILE_FLAGS -std=gnu++20)
    else()
      message(STATUS "ns3 v${NS3_VERSION} does not require C++20.")
    endif()
    set(SIMGRID_HAVE_NS3 ON)
    set(SIMGRID_DEP "${SIMGRID_DEP} ${NS3_LIBRARIES}")
  else()
    message(FATAL_ERROR "Cannot find ns-3. Please install it (apt-get install ns3 libns3-dev) or disable that cmake option")
  endif()
endif()

### Check for Eigen library
if ((NOT DEFINED EIGEN3_HINT) OR (NOT EIGEN3_HINT STRLESS_EQUAL "OFF"))
  set(SIMGRID_HAVE_EIGEN3 OFF)
  find_package (Eigen3 3.3 CONFIG
                HINTS ${EIGEN3_HINT})
  if (Eigen3_FOUND)
    set(SIMGRID_HAVE_EIGEN3 ON)
    message(STATUS "Found Eigen3: ${EIGEN3_INCLUDE_DIR}")
    include_directories(${EIGEN3_INCLUDE_DIR})
    if ("3.3.4" VERSION_EQUAL EIGEN3_VERSION_STRING AND CMAKE_COMPILER_IS_GNUCC)
      message(STATUS "Avoid build error of Eigen3 v3.3.4 using -Wno-error=int-in-bool-context")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=int-in-bool-context")
    endif()
  else()
    message(STATUS "Disabling model BMF because Eigen3 was not found. If it's installed, use EIGEN3_HINT to hint cmake about the location of Eigen3Config.cmake")
  endif()
  mark_as_advanced(Eigen3_DIR)
else()
  message(STATUS "Disabling Eigen3 as requested by the user (EIGEN3_HINT is set to 'OFF')")
endif()

# Check for our JSON dependency
set(SIMGRID_HAVE_JSON 0)
find_package(nlohmann_json 3.7
             HINTS ${nlohmann_json_HINT})
if (nlohmann_json_FOUND)
  set(SIMGRID_HAVE_JSON 1)
  if (NOT NLOHMANN_JSON_INCLUDE_DIR)
    get_target_property(NLOHMANN_JSON_INCLUDE_DIR nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
    list(REMOVE_DUPLICATES NLOHMANN_JSON_INCLUDE_DIR)
  else()
    include_directories(${NLOHMANN_JSON_INCLUDE_DIR})
  endif()
  message(STATUS "Found nlohmann_json: ${NLOHMANN_JSON_INCLUDE_DIR}")
endif()
mark_as_advanced(nlohmann_json_DIR)

set(HAVE_PAPI OFF)
if(enable_smpi_papi)
  include(FindPAPI)
  if (NOT HAVE_PAPI)
    message(FATAL_ERROR "Cannot find PAPI. Please install it (apt-get install papi-tools libpapi-dev) or disable PAPI bindings.")
  endif()
endif()
mark_as_advanced(PAPI_PREFIX)

# But we do need the core of Boost
# cmake before 3.13.1 does not know about stacktrace components. Fix it.
# Usable components: https://www.boost.org/doc/libs/1_65_1/doc/html/stacktrace/configuration_and_build.html
set(_Boost_STACKTRACE_HEADERS           "boost/stacktrace.hpp")
set(_Boost_STACKTRACE_BACKTRACE_HEADERS "boost/stacktrace.hpp")
set(_Boost_STACKTRACE_ADDR2LINE_HEADERS "boost/stacktrace.hpp")

  if(minimal-bindings) # When we want a minimal python or Java library, don't even search for boost optional components
    message(STATUS "Don't even look for boost optional components, as we build minimal binding libraries.")
    find_package(Boost 1.48)
  else()
    find_package(Boost 1.59 OPTIONAL_COMPONENTS context stacktrace_backtrace stacktrace_addr2line)
  endif()
  if(Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
    message(STATUS "Mandatory components found. SimGrid is compilable.")
    if (NOT minimal-bindings)
      message(STATUS "Looking for optional Boost components:")
      set(Boost_FOUND 1) # These components are optionals
      CHECK_INCLUDE_FILE("backtrace.h" HAVE_BACKTRACE_H) # check that backtrace is actually possible
      if (Boost_STACKTRACE_BACKTRACE_FOUND AND HAVE_BACKTRACE_H)
        message (STATUS "  stacktrace: found the fast 'backtrace' implementation. Activating human-readable stack traces.")
        set(HAVE_BOOST_STACKTRACE_BACKTRACE 1)
      else()
        set(HAVE_BOOST_STACKTRACE_BACKTRACE 0)
        if (Boost_STACKTRACE_ADDR2LINE_FOUND)
          message (STATUS "  stacktrace: found the slow 'addr2line' implementation. Activating human-readable stack traces.")
          set(HAVE_BOOST_STACKTRACE_ADDR2LINE 1)
        else()
          message (STATUS "  stacktrace: MISSING. Install libboost-stacktrace-dev to display the stacktraces.")
          set(HAVE_BOOST_STACKTRACE_ADDR2LINE 0)
        endif()
      endif()

      if(Boost_CONTEXT_FOUND)
        message (STATUS "  context: found. Activating Boost contexts.")
        set(HAVE_BOOST_CONTEXTS ON)
      else()
        message (STATUS "  context: MISSING. Install libboost-context-dev for this optional feature.")
        set(HAVE_BOOST_CONTEXTS OFF)
      endif()
    endif()
  else()
    if(APPLE)
      message(FATAL_ERROR "Boost libraries not found. Try to install them with 'sudo fink install boost1.53.nopython' (check the exact name with 'fink list boost') or 'sudo brew install boost'")
    else()
      find_package(Boost 1.48) #try without optional libraries
      if(NOT Boost_FOUND)
        message(FATAL_ERROR "Boost libraries not found. Install libboost-dev (>= 1.48.0).")
      else()
        include_directories(${Boost_INCLUDE_DIRS})
        message(STATUS "Mandatory components found. SimGrid is compilable.")
      endif()
    endif()
  endif()
mark_as_advanced(Boost_CONTEXT_LIBRARY_RELEASE)
mark_as_advanced(Boost_INCLUDE_DIR)
mark_as_advanced(Boost_STACKTRACE_ADDR2LINE_LIB)
mark_as_advanced(Boost_STACKTRACE_BACKTRACE_LIB)

# Checks for header libraries functions.
CHECK_LIBRARY_EXISTS(rt      clock_gettime           "" HAVE_POSIX_GETTIME)
CHECK_LIBRARY_EXISTS(pthread pthread_setaffinity_np  "" HAVE_PTHREAD_SETAFFINITY)
CHECK_INCLUDE_FILE("pthread_np.h" HAVE_PTHREAD_NP_H) # for pthread_setaffinity_np() on FreeBSD

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  set(CMAKE_REQUIRED_DEFINITIONS "-D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE")
else()
  set(CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")
endif()

CHECK_INCLUDE_FILE("valgrind/valgrind.h" HAVE_VALGRIND_H)
CHECK_INCLUDE_FILE("unistd.h" HAVE_UNISTD_H)
CHECK_INCLUDE_FILE("linux/futex.h" HAVE_FUTEX_H)

CHECK_FUNCTION_EXISTS(dlfunc HAVE_DLFUNC)
CHECK_FUNCTION_EXISTS(gettimeofday HAVE_GETTIMEOFDAY)
CHECK_FUNCTION_EXISTS(nanosleep HAVE_NANOSLEEP)
CHECK_FUNCTION_EXISTS(sysconf HAVE_SYSCONF)
if(NOT HAVE_SYSCONF)
  message(FATAL_ERROR "Cannot build without sysconf.")
endif()
CHECK_FUNCTION_EXISTS(process_vm_readv HAVE_PROCESS_VM_READV)
CHECK_FUNCTION_EXISTS(mremap HAVE_MREMAP)

CHECK_SYMBOL_EXISTS(vasprintf stdio.h HAVE_VASPRINTF)

CHECK_INCLUDE_FILE("sys/sendfile.h" HAVE_SENDFILE_H)
CHECK_FUNCTION_EXISTS(sendfile HAVE_SENDFILE)
if(HAVE_SENDFILE_H AND HAVE_SENDFILE)
  set(SG_HAVE_SENDFILE 1)
else()
  set(SG_HAVE_SENDFILE 0)
endif()

if(enable_mallocators)
  SET(SIMGRID_HAVE_MALLOCATOR 1)
else()
  SET(SIMGRID_HAVE_MALLOCATOR 0)
endif()

SET(SIMGRID_HAVE_MC OFF)

if(enable_model-checking)
  message(STATUS "Enabling the stateless model-checking as requested.")
  SET(SIMGRID_HAVE_MC ON)
endif()

if(enable_smpi)
  SET(HAVE_SMPI ON)
  SET(HAVE_PRIVATIZATION ON)
else()
  SET(HAVE_SMPI OFF)
endif()

#--------------------------------------------------------------------------------------------------
### Check what context backends are available

set(HAVE_UCONTEXT_CONTEXTS 0)
CHECK_INCLUDE_FILE("ucontext.h" HAVE_UCONTEXT_H)
if(NOT HAVE_UCONTEXT_H)
  message(STATUS "No ucontext factory: <ucontext.h> not found.")
elseif(APPLE)
  message(STATUS "No ucontext factory: Apple don't want us to use them.")
  set(HAVE_UCONTEXT_H 0)
else()
  try_compile(compile_makecontext ${CMAKE_BINARY_DIR} ${CMAKE_HOME_DIRECTORY}/tools/cmake/test_prog/prog_makecontext.c
    OUTPUT_VARIABLE compile_makecontext_output)

  #If can have both context
  if(NOT compile_makecontext)
    message(STATUS "Error: <ucontext.h> exists, but makecontext is not compilable. Compilation output:\n ${compile_makecontext_output}")
    message(STATUS "No ucontext factory: makecontext() is not compilable.")
  else()
    message(STATUS "Support for ucontext factory ok.")
    set(HAVE_UCONTEXT_CONTEXTS 1)

    # Stack setup (size and address)
    try_run(RUN_makecontext_VAR COMPILE_makecontext_VAR
      ${CMAKE_BINARY_DIR} ${CMAKE_HOME_DIRECTORY}/tools/cmake/test_prog/prog_stacksetup.c
      RUN_OUTPUT_VARIABLE stack_setup)

    LIST(LENGTH stack_setup stack_setup_len)
    if("${stack_setup_len}" STREQUAL "2")
      LIST(GET stack_setup 0 makecontext_addr)
      LIST(GET stack_setup 1 makecontext_size)
      set(sg_makecontext_stack_addr "#define sg_makecontext_stack_addr(skaddr) (${makecontext_addr})")
      set(sg_makecontext_stack_size "#define sg_makecontext_stack_size(sksize) (${makecontext_size})")
    else()
      message(FATAL_ERROR "Could not figure out the stack setup. Compil: ${RUN_makecontext_VAR}. Exec: ${COMPILE_makecontext_VAR}. Output: ${stack_setup}")
    endif()
  endif()
endif()

# Stack growth direction (upward or downward). Used for the following contexts: SysV, raw, Boost
try_run(RUN_stackgrowth_VAR COMPILE_stackgrowth_VAR
  ${CMAKE_BINARY_DIR}
  ${CMAKE_HOME_DIRECTORY}/tools/cmake/test_prog/prog_stackgrowth.c
  RUN_OUTPUT_VARIABLE stack
  COPY_FILE test_stackgrowth)

if("${stack}" STREQUAL "down")
  set(PTH_STACKGROWTH "-1")
elseif("${stack}" STREQUAL "up")
  set(PTH_STACKGROWTH "1")
else()
  if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    set(PTH_STACKGROWTH "-1")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
    set(PTH_STACKGROWTH "-1")
  else()
    message(FATAL_ERROR "Could not figure out the stack direction. Test prog returned: ${stack}; CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}.")
  endif()
endif()
# If the test ran well, remove the test binary
file(REMOVE ${CMAKE_BINARY_DIR}/test_stackgrowth)
#--------------------------------------------------------------------------------------------------

###############
## GIT version check
##
if(EXISTS ${CMAKE_HOME_DIRECTORY}/.git/)
  execute_process(COMMAND git rev-parse --verify --short HEAD
     WORKING_DIRECTORY ${CMAKE_HOME_DIRECTORY}
     OUTPUT_VARIABLE GIT_VERSION
     OUTPUT_STRIP_TRAILING_WHITESPACE)
  # Check for uncommitted changes
  execute_process(COMMAND git diff --name-only HEAD
    WORKING_DIRECTORY ${CMAKE_HOME_DIRECTORY}
    OUTPUT_VARIABLE files_changed)
  if(files_changed)
    set(GIT_VERSION "${GIT_VERSION}-dirty")
  endif()
elseif(EXISTS ${CMAKE_HOME_DIRECTORY}/.gitversion)
  FILE(STRINGS ${CMAKE_HOME_DIRECTORY}/.gitversion GIT_VERSION)
else()
  set(GIT_VERSION "none, release version")
endif()
message(STATUS "Git version: ${GIT_VERSION}")

### Define source packages for Libs
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/DefinePackages.cmake)

### Setup gcc & clang flags (include after DefinePackage.cmake, and before generating header files)
if (NOT MSVC)
  include(${CMAKE_HOME_DIRECTORY}/tools/cmake/Flags.cmake)
endif()

### Generate the required scripts
#################################

# We need two versions of the SMPI scripts because they contain the path to the library
# so, it depends of whether SimGrid is installed, or run from the sources (during the build)

file(READ ${CMAKE_HOME_DIRECTORY}/src/smpi/smpitools.sh SMPITOOLS_SH) # Definitions shared amongst all SMPI scripts, inlined in each of them

### SMPI script used when simgrid is installed
set(exec_prefix ${CMAKE_INSTALL_PREFIX})
set(includedir "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
set(libdir "${CMAKE_INSTALL_FULL_LIBDIR}")
set(includeflag "\"-I${includedir}\" \"-I${includedir}/smpi\"")
set(CMAKE_SMPI_COMMAND "export LD_LIBRARY_PATH=\"${libdir}")
if(NS3_LIBRARY_PATH)
  set(CMAKE_SMPI_COMMAND "${CMAKE_SMPI_COMMAND}:${NS3_LIBRARY_PATH}")
endif()
set(CMAKE_SMPI_COMMAND "${CMAKE_SMPI_COMMAND}\${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"")
set(SMPIMAIN ${libdir}/simgrid/smpimain)
set(SMPIREPLAYMAIN ${libdir}/simgrid/smpireplaymain)

configure_file(${CMAKE_HOME_DIRECTORY}/include/smpi/mpif.h.in ${CMAKE_BINARY_DIR}/include/smpi/mpif.h @ONLY)
#configure mpif.f90 to build mpi.mod
if(SMPI_FORTRAN)
  set(MODULE_MPIF_IN "module mpi")
  set(MODULE_MPIF_OUT "end module mpi")
  configure_file(${CMAKE_HOME_DIRECTORY}/include/smpi/mpif.h.in ${CMAKE_BINARY_DIR}/src/smpi/mpif.f90.generated @ONLY)
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/src/smpi/mpif.f90.generated ${CMAKE_BINARY_DIR}/src/smpi/mpif.f90)
  file(REMOVE ${CMAKE_BINARY_DIR}/src/smpi/mpif.f90.generated)
  set(CMAKE_Fortran_MODULE_DIRECTORY ${CMAKE_BINARY_DIR}/include/smpi)
  add_library(mpi SHARED ${CMAKE_BINARY_DIR}/src/smpi/mpif.f90)
endif()

foreach(script cc cxx ff f90 run)
  configure_file(${CMAKE_HOME_DIRECTORY}/src/smpi/smpi${script}.in ${CMAKE_BINARY_DIR}/bin/smpi${script} @ONLY)
endforeach()

### SMPI scripts used when compiling simgrid
set(exec_prefix "${CMAKE_BINARY_DIR}/smpi_script/")
set(includedir "${CMAKE_HOME_DIRECTORY}/include")
set(libdir "${CMAKE_BINARY_DIR}/lib")
set(includeflag "\"-I${includedir}\" \"-I${includedir}/smpi\"")
set(includeflag "${includeflag} \"-I${CMAKE_BINARY_DIR}/include\" \"-I${CMAKE_BINARY_DIR}/include/smpi\"")
set(CMAKE_SMPI_COMMAND "export LD_LIBRARY_PATH=\"${libdir}")
if(NS3_LIBRARY_PATH)
  set(CMAKE_SMPI_COMMAND "${CMAKE_SMPI_COMMAND}:${NS3_LIBRARY_PATH}")
endif()
set(CMAKE_SMPI_COMMAND "${CMAKE_SMPI_COMMAND}\${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"")
set(SMPIMAIN ${libdir}/simgrid/smpimain)
set(SMPIREPLAYMAIN ${libdir}/simgrid/smpireplaymain)

foreach(script cc cxx ff f90 run)
  configure_file(${CMAKE_HOME_DIRECTORY}/src/smpi/smpi${script}.in ${CMAKE_BINARY_DIR}/smpi_script/bin/smpi${script} @ONLY)
endforeach()

foreach(script cc cxx ff f90 run)
  execute_process(COMMAND chmod a=rwx ${CMAKE_BINARY_DIR}/bin/smpi${script})
  execute_process(COMMAND chmod a=rwx ${CMAKE_BINARY_DIR}/smpi_script/bin/smpi${script})
endforeach()

set(generated_headers_to_install
  ${CMAKE_CURRENT_BINARY_DIR}/include/smpi/mpif.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/simgrid/config.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/simgrid/version.h
  )

set(generated_headers  ${CMAKE_CURRENT_BINARY_DIR}/src/internal_config.h )

set(generated_files_to_clean
  ${generated_headers}
  ${generated_headers_to_install}
  ${CMAKE_BINARY_DIR}/bin/smpicc
  ${CMAKE_BINARY_DIR}/bin/smpicxx
  ${CMAKE_BINARY_DIR}/bin/smpiff
  ${CMAKE_BINARY_DIR}/bin/smpif90
  ${CMAKE_BINARY_DIR}/bin/smpirun
  ${CMAKE_BINARY_DIR}/bin/simgrid_update_xml
  ${CMAKE_BINARY_DIR}/examples/smpi/tracing/smpi_traced.trace
  )

if(NOT "${CMAKE_BINARY_DIR}" STREQUAL "${CMAKE_HOME_DIRECTORY}")
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions0.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions0.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions1.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions1.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_allreduce.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_allreduce.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_barrier.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_barrier.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_bcast.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_bcast.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_with_isend.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_with_isend.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_alltoall.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_alltoall.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_alltoallv.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_alltoallv.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_waitall.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_waitall.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_reducescatter.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_reducescatter.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_gather.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_gather.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay/actions_allgatherv.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_allgatherv.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/teshsuite/smpi/hostfile ${CMAKE_BINARY_DIR}/teshsuite/smpi/hostfile COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/teshsuite/smpi/hostfile_cluster ${CMAKE_BINARY_DIR}/teshsuite/smpi/hostfile_cluster COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/teshsuite/smpi/hostfile_griffon ${CMAKE_BINARY_DIR}/teshsuite/smpi/hostfile_griffon COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/teshsuite/smpi/hostfile_coll ${CMAKE_BINARY_DIR}/teshsuite/smpi/hostfile_coll COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/teshsuite/smpi/hostfile_io ${CMAKE_BINARY_DIR}/teshsuite/smpi/hostfile_io COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/teshsuite/smpi/hostfile_empty ${CMAKE_BINARY_DIR}/teshsuite/smpi/hostfile_empty COPYONLY)

  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/description_file ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/description_file COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/README ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/README COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/smpi_replay.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/smpi_replay.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace0.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace0.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace1.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace1.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace2.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace2.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace3.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace3.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace4.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace4.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace5.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace5.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace6.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace6.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace7.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace7.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace8.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace8.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace9.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace9.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace10.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace10.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace11.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace11.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace12.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace12.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace13.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace13.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace14.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace14.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace15.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace15.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace16.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace16.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace17.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace17.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace18.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace18.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace19.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace19.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace20.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace20.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace21.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace21.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace22.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace22.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace23.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace23.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace24.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace24.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace25.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace25.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace26.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace26.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace27.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace27.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace28.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace28.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace29.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace29.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace30.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace30.txt COPYONLY)
  configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace31.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace31.txt COPYONLY)

configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/compute_only.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/compute_only.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/compute_only/actions0.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/compute_only/actions0.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/compute_only/actions1.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/compute_only/actions1.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/empty.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/empty.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/empty/actions0.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/empty/actions0.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/empty/actions1.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/empty/actions1.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/mixed.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/mixed.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/mixed/actions0.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/mixed/actions0.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/mixed/actions1.txt ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/mixed/actions1.txt COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/workload_compute ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/workload_compute_consecutive ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute_consecutive COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/workload_compute_consecutive2 ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute_consecutive2 COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/workload_compute_simple ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute_simple COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2_same_time ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2_same_time COPYONLY)
configure_file(${CMAKE_HOME_DIRECTORY}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2_same_time_and_resources ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2_same_time_and_resources COPYONLY)

  set(generated_files_to_clean
    ${generated_files_to_clean}
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions0.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions1.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_allreduce.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_barrier.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_bcast.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_with_isend.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_alltoall.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_alltoallv.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_waitall.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_gather.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_allgatherv.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay/actions_reducescatter.txt
    ${CMAKE_BINARY_DIR}/teshsuite/smpi/hostfile
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/description_file
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/README
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/smpi_replay.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace0.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace1.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace2.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace3.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace4.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace5.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace6.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace7.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace8.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace9.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace10.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace11.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace12.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace13.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace14.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace15.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace16.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace17.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace18.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace19.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace20.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace21.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace22.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace23.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace24.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace25.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace26.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace27.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace28.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace29.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace30.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple/ti_traces_32_1/ti_trace31.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/compute_only.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/compute_only/actions0.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/compute_only/actions1.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/empty.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/empty/actions0.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/empty/actions1.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/mixed.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/mixed/actions0.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/mixed/actions1.txt
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/replay_multiple_manual.tesh
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute_consecutive
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute_consecutive2
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_compute_simple
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_empty1
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_empty2
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_empty2_same_resources
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_empty2_same_time
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_empty2_same_time_and_resources
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_mixed1
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2_same_resources
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2_same_time
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_mixed2_same_time_and_resources
    ${CMAKE_BINARY_DIR}/examples/smpi/replay_multiple_manual_deploy/workload_nojob
    )
endif()

SET_DIRECTORY_PROPERTIES(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES
  "${generated_files_to_clean}")

add_custom_target(tests    COMMENT "Recompiling the tests")
add_custom_target(tests-mc COMMENT "Recompiling the MC tests and tools.")
add_dependencies(tests tests-mc)
add_custom_target(tests-ns3 COMMENT "Recompiling the ns3 tests and tools.")
add_dependencies(tests tests-ns3)
add_custom_target(examples COMMENT "Recompiling all examples")
add_dependencies(examples tests)

### Build some Maintainer files
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/MaintainerMode.cmake)

### Make Libs
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/MakeLib.cmake)

if((NOT DEFINED enable_java) OR enable_java)
  find_package(JNI)
  if (NOT ${JNI_FOUND})
    if(enable_java)
      message(FATAL_ERROR "JNI not found. Please install it, or disable the 'enable_java' option.")
    else()
      message(status "JNI not found, thus disabling Java.")
    endif()

  else()
   find_package(Java 1.8 COMPONENTS Runtime Development)
   if (NOT ${Java_FOUND})
     if(enable_java)
       message(FATAL_ERROR "Java not found (need at least Java 8). Please install the JDK, or disable the 'enable_java' option.")
     else()
       message(status "Java not found (need at least Java 8) thus disabled.")
     endif()
    else() # We have both Java and JNI
      message(STATUS "Java version: ${Java_VERSION_STRING}")
      message(STATUS "Java compiler: ${Java_JAVAC_EXECUTABLE}; Runtime: ${Java_JAVA_EXECUTABLE}")
      message(STATUS "Java: JNI include dirs: ${JNI_INCLUDE_DIRS}")
      set(SIMGRID_HAVE_JAVA 1)
      
      include(${CMAKE_HOME_DIRECTORY}/tools/cmake/Java.cmake)
    endif()
    
  endif()
else()
  message(STATUS "Java disabled by user request (remove -Denable_java=${enable_java} if it's not what you want).")
endif()

# Python binding (with pybind11)
################
if((NOT DEFINED enable_python) OR enable_python)
  if(EXISTS ${CMAKE_HOME_DIRECTORY}/pybind11) # Try to use a local copy of pybind11, if any
    message(STATUS "Use the internal copy of pybind11.")
    add_subdirectory(${CMAKE_HOME_DIRECTORY}/pybind11)
    set(pybind11_FOUND ON)

    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_HOME_DIRECTORY}/pybind11/tools/)
    set(Python_ADDITIONAL_VERSIONS 3.9 3.8 3.7 3.6 3.5 3.4)
    find_package(PythonLibsNew ${PYBIND11_PYTHON_VERSION} REQUIRED)

  else()
    find_package(pybind11 CONFIG)
    message(STATUS "Pybind11 version: ${pybind11_VERSION}")
    if (pybind11_VERSION VERSION_LESS 2.4)
      message(STATUS "SimGrid needs at least v2.4 of pybind11. Disabling the Python bindings.")
      set(pybind11_FOUND OFF)
    endif()
  endif()
endif()

find_package(Python3 COMPONENTS Development)
if(NOT Python3_Development_FOUND OR NOT pybind11_FOUND)
  message(STATUS "SimGrid Python bindings cannot be built on this system.")
  set(default_enable_python OFF)
else()
  set(default_enable_python ON)
endif()

option(enable_python "Whether the Python bindings are activated." ${default_enable_python}) # ON by default if dependencies are met

if(enable_python)
  if(NOT Python3_Development_FOUND)
    message(FATAL_ERROR "Please install the development components of Python (python3-dev on Debian) to build the Python bindings (or disable that option).")
  endif()
  if(pybind11_FOUND)
    message(STATUS "Python bindings will be built.")
    if(NOT enable_lto)
      set(pybind11_options NO_EXTRAS)
    endif()
    set(python_files ${CMAKE_SOURCE_DIR}/src/bindings/python/simgrid_python.cpp)

    find_package(Python3 COMPONENTS NumPy)

    if (((NOT DEFINED enable_smpi) OR enable_smpi) AND Python3_NumPy_FOUND)
      set(python_files ${python_files} ${CMAKE_SOURCE_DIR}/src/bindings/python/smpi_python.cpp)
      add_compile_definitions(SIMGRID_HAVE_PYTHON_SMPI)
      set(SIMGRID_HAVE_PYTHON_SMPI 1)
      message(STATUS "SMPI Python bindings will be built.")
    else()
      message(STATUS "SMPI Python bindings are not built. They may be disabled, or NumPy is missing.")
    endif()

    message(STATUS "Files to compile: ${python_files}")
    pybind11_add_module(python-bindings
                        ${python_files}
                        ${pybind11_options})

    target_compile_features(python-bindings PRIVATE cxx_std_14)
    target_link_libraries(python-bindings PUBLIC simgrid)
    set_target_properties(python-bindings PROPERTIES
                          LIBRARY_OUTPUT_NAME simgrid
                          CXX_VISIBILITY_PRESET "default"
                          INTERPROCEDURAL_OPTIMIZATION FALSE)
    # LTO is disabled here from the python bindings because this makes a
    # cmake warning about CMP0069 even when this policy is set. This
    # problem may be in cmake, in pybind11 or even in our code, not sure.
    # It may get eventually solved in cmake or pybind11. Or not.
    # The sure thing is that our python bindings are in one file only,
    # so there is no need for LTO here. Problem solved :)

    add_dependencies(tests python-bindings)
    set_property(TARGET python-bindings
                 APPEND PROPERTY INCLUDE_DIRECTORIES "${INTERNAL_INCLUDES}")

    if("${SIMGRID_PYTHON_LIBDIR}" STREQUAL "") # value not manually set
      if("${CMAKE_INSTALL_PREFIX}" STREQUAL "/usr")
        set(SIMGRID_PYTHON_LIBDIR ${Python3_SITEARCH})
      else("${CMAKE_INSTALL_PREFIX}" STREQUAL "/usr")
        string(REGEX REPLACE "^/usr/" "${CMAKE_INSTALL_PREFIX}/" SIMGRID_PYTHON_LIBDIR ${Python3_SITEARCH})
      endif("${CMAKE_INSTALL_PREFIX}" STREQUAL "/usr")
    endif()
    install(TARGETS python-bindings
      	    LIBRARY DESTINATION "${SIMGRID_PYTHON_LIBDIR}")
  else()
    message(FATAL_ERROR "Please install pybind11-dev to build the Python bindings (or disable that option).")
  endif()
endif()
mark_as_advanced(PYBIND11_PYTHON_VERSION)
mark_as_advanced(pybind11_DIR)

### Make tests
if(enable_memcheck_xml)
  set(enable_memcheck true)
endif()

INCLUDE(CTest)
ENABLE_TESTING()
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/Tests.cmake)
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/CTestConfig.cmake)

### Define subdirectories
foreach(cmakefile ${CMAKEFILES_TXT})
  string(REPLACE "/CMakeLists.txt" "" repository ${cmakefile})
  add_subdirectory("${CMAKE_HOME_DIRECTORY}/${repository}")
endforeach()

### Setup the distrib
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/Distrib.cmake)

### Build the docs if asked to
include(${CMAKE_HOME_DIRECTORY}/tools/cmake/Documentation.cmake)

### Generate the required headers
#################################

# Avoid triggering a (full) rebuild by touching the files if they did not really change
configure_file("${CMAKE_HOME_DIRECTORY}/src/internal_config.h.in"    "${CMAKE_BINARY_DIR}/src/internal_config.h.generated"    @ONLY IMMEDIATE)
configure_file("${CMAKE_HOME_DIRECTORY}/include/simgrid/version.h.in" "${CMAKE_BINARY_DIR}/include/simgrid/version.h.generated" @ONLY IMMEDIATE)
configure_file("${CMAKE_HOME_DIRECTORY}/include/simgrid/config.h.in" "${CMAKE_BINARY_DIR}/include/simgrid/config.h.generated" @ONLY IMMEDIATE)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/src/internal_config.h.generated ${CMAKE_BINARY_DIR}/src/internal_config.h)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/include/simgrid/version.h.generated ${CMAKE_BINARY_DIR}/include/simgrid/version.h)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/include/simgrid/config.h.generated ${CMAKE_BINARY_DIR}/include/simgrid/config.h)
file(REMOVE ${CMAKE_BINARY_DIR}/src/internal_config.h.generated)
file(REMOVE ${CMAKE_BINARY_DIR}/include/simgrid/config.h.generated)
file(REMOVE ${CMAKE_BINARY_DIR}/include/simgrid/version.h.generated)

### Print the result of configuration
message("")
message("##########################################")
message("#### Content of src/internal_config.h ####")
message("##########################################")
file(STRINGS ${CMAKE_CURRENT_BINARY_DIR}/src/internal_config.h config_output)
LIST(REMOVE_AT config_output 0 1 2 3 4 5 6 7 8 9 10) # Pass the file header
foreach(line ${config_output})
  message("   ${line}")
endforeach()
message("##########################################")
message("####   Content of simgrid/config.h    ####")
message("##########################################")
file(STRINGS ${CMAKE_CURRENT_BINARY_DIR}/include/simgrid/config.h config_output)
LIST(REMOVE_AT config_output 0 1 2 3 4 5 6 7 8 9 -1) # Pass the file header
foreach(line ${config_output})
  message("   ${line}")
endforeach()
message("##########################################")
message("####   End of configuration headers   ####")
message("##########################################")

message("\nConfiguration of package `simgrid':")
message("        Home directory ..............: ${CMAKE_HOME_DIRECTORY}")
message("        Build Name ..................: ${BUILDNAME}")
message("        Cmake Generator .............: ${CMAKE_GENERATOR}")
message("        Site ........................: ${SITE}")
message("        Install prefix ..............: ${CMAKE_INSTALL_PREFIX}")
message("        sthread path ................: ${STHREAD_PATH}")
message("        Release .....................: simgrid-${release_version}")
message("")
message("        Compiler: C .................: ${CMAKE_C_COMPILER} (id: ${CMAKE_C_COMPILER_ID})")
message("                version .............: ${CMAKE_C_COMPILER_VERSION}")
message("                is gnu ..............: ${CMAKE_COMPILER_IS_GNUCC}")
message("        Compiler: C++ ...............: ${CMAKE_CXX_COMPILER} (id: ${CMAKE_CXX_COMPILER_ID})")
message("                version .............: ${CMAKE_CXX_COMPILER_VERSION}")
if(${SIMGRID_HAVE_JAVA})
  message("        Compiler: Javac .............: ${Java_JAVAC_EXECUTABLE}")
  message("                version .............: ${Java_VERSION_STRING}")
  message("                runtime .............: ${Java_JAVA_EXECUTABLE}")
else()
  message("        Compiler: Javac .............: (java disabled or not found -- check the logs)")
endif()
if(CMAKE_Fortran_COMPILER)
  message("        Compiler: Fortran ...........: ${SMPI_Fortran_COMPILER} (id: ${CMAKE_Fortran_COMPILER_ID})")
  message("                version .............: ${CMAKE_Fortran_COMPILER_VERSION}")
endif()
message("        Linker: .....................: ${CMAKE_LINKER}")
message("                version .............: ${LINKER_VERSION}")
message("        Make program: ...............: ${CMAKE_MAKE_PROGRAM}")
message("")
message("        CFlags ......................: ${CMAKE_C_FLAGS}")
message("        CXXFlags ....................: ${CMAKE_CXX_FLAGS}")
message("        LDFlags .....................: ${CMAKE_C_LINK_FLAGS}")
message("        with LTO ....................: ${enable_lto}")
message("")

if (SIMGRID_HAVE_NS3)
  message("        Compile ns-3 ................: ON (path: ${NS3_PATH})")
else()
  message("        Compile ns-3 ................: OFF  (hint: ${NS3_HINT})")
endif()

if(pybind11_FOUND)
  message("        Compile Python bindings .....: ${enable_python}")
  message("          module ....................: ${PYTHON_MODULE_PREFIX}simgrid${PYTHON_MODULE_EXTENSION}")
  message("          install path ..............: ${SIMGRID_PYTHON_LIBDIR} (force another value with -DSIMGRID_PYTHON_LIBDIR)")
  if(SIMGRID_HAVE_PYTHON_SMPI)
    message("          MPI bindings ..............: ON")
  else()
    message("          MPI bindings ..............: OFF")
  endif()
else()
  message("        Compile Python bindings .....: OFF (disabled, or pybind11 not found)")
endif()
if(SIMGRID_HAVE_EIGEN3)
  message("        Eigen3 library ..............: ${EIGEN3_VERSION_STRING} in ${EIGEN3_INCLUDE_DIR}")
else()
  message("        Eigen3 library ..............: not found (EIGEN3_HINT='${EIGEN3_HINT}')")
endif()
if(SIMGRID_HAVE_JSON)
  message("        JSON library ................: ${nlohmann_json_VERSION} in ${NLOHMANN_JSON_INCLUDE_DIR}")
else()
  message("        JSON library ................: not found (nlohmann_json_HINT='${nlohmann_json_HINT}')")
endif()
message("        Compile Smpi ................: ${HAVE_SMPI}")
message("          Smpi fortran ..............: ${SMPI_FORTRAN}")
message("          MPICH3 testsuite ..........: ${enable_testsuite_smpi_MPICH3}")
message("          MBI testsuite .............: ${enable_testsuite_smpi_MBI}")
message("          Privatization .............: ${HAVE_PRIVATIZATION}")
message("          PAPI support ..............: ${HAVE_PAPI}")
message("        Compile Boost.Context support: ${HAVE_BOOST_CONTEXTS}")
message("")
message("        Model checking ..............: ${SIMGRID_HAVE_MC}")
message("          MBI testsuite .............: ${enable_testsuite_smpi_MBI}")
message("          McMini testsuite ..........: ${enable_testsuite_McMini}")
message("")
message("        Maintainer mode .............: ${enable_maintainer_mode}")
message("        Documentation ...............: ${enable_documentation}")
message("        Graphviz mode ...............: ${HAVE_GRAPHVIZ}")
message("        Mallocators .................: ${enable_mallocators}")
message("")
message("        SimGrid dependencies ........: ${SIMGRID_DEP}")
message("")

execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/Testing/Notes/)
file(WRITE ${PROJECT_BINARY_DIR}/Testing/Notes/Build  "GIT version : ${GIT_VERSION}\n")
file(APPEND ${PROJECT_BINARY_DIR}/Testing/Notes/Build "Release     : simgrid-${release_version}\n")

