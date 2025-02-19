.. Copyright 2005-2024

.. _install:

Installing SimGrid
==================


SimGrid should work out of the box on Linux, macOS, FreeBSD, and
Windows (with WSL).

Pre-compiled Packages
---------------------

Binaries for Linux
^^^^^^^^^^^^^^^^^^

To get all of SimGrid on Debian or Ubuntu, simply type one of the
following lines, or several lines if you need several languages.

.. code-block:: console

   $ apt install libsimgrid-dev  # if you want to develop in C or C++
   $ apt install python3-simgrid # if you want to develop in Python

If you use the Nix_ package manager, the latest SimGrid release is packaged as ``simgrid`` in Nixpkgs_.
Previous SimGrid versions are maintained in `NUR-Kapack`_ and are available
pre-compiled in release and debug modes on the `capack cachix binary cache`_
— refer to `NUR-Kapack's documentation`_ for usage instructions.

If you use a pacman-based system (*e.g.*, Arch Linux and derived distributions),
the latest SimGrid is available in the `simgrid AUR package`_
— refer to `AUR official documentation`_ for installation instructions.

If you build pre-compiled packages for other distributions, drop us an
email.

.. _Nix: https://nixos.org/
.. _Nixpkgs: https://github.com/NixOS/nixpkgs
.. _NUR-Kapack: https://github.com/oar-team/nur-kapack
.. _capack cachix binary cache: https://app.cachix.org/cache/capack
.. _NUR-Kapack's documentation: https://github.com/oar-team/nur-kapack
.. _simgrid AUR package: https://aur.archlinux.org/packages/simgrid/
.. _AUR official documentation: https://wiki.archlinux.org/title/Arch_User_Repository

Binaries from macOS
^^^^^^^^^^^^^^^^^^^

SimGrid can be found in the Homebrew package manager. Troubleshooting:

warning: dylib (libsimgrid.dylib) was built for newer macOS version (14.0) than being linked (13.3)
  This was reported with the SimGrid version from Homebrew on a Mac book air M1 (ARM).
  The solution is simply to export this variable before the compilation of your binaries:

  ``export MACOSX_DEPLOYMENT_TARGET=14.0``

.. _deprecation_policy:

Version numbering and deprecation
---------------------------------

SimGrid tries to be both a research instrument that you can trust, and
a vivid project targeting the future issues. We have 4 stable versions
per year, numbered 3.24 or 3.25. Backward compatibility is ensured for
one year: Code compiling without warning on 3.24 will still compile
with 3.28, but maybe with some deprecation warnings. You should update
your SimGrid installation at least once a year and fix those
deprecation warnings: the compatibility wrappers are usually removed
after 4 versions. Another approach is to never update your SimGrid
installation, but we don't provide any support to old versions.

Interim versions (also called pre-versions) may be released between
stable releases. They are numbered 3.X.Y, with even Y (for example,
3.23.2 was released on July 8. 2019 as a pre-version of 3.24). These
versions should be as usable as regular stable releases, even if they
may be somewhat less tested and documented. They play no role in our
deprecation handling, and they are not really announced to not spam
our users.

Version numbered 3.X.Y with odd Y are git versions. They often work,
but no guarantee is given whatsoever (all releases are given "as is",
but that's even more so for these unreleased versions).

.. _install_src:

Installing from the Source
--------------------------

.. _install_src_deps:

Getting the Dependencies
^^^^^^^^^^^^^^^^^^^^^^^^

C++ compiler (either g++, clang, or icc).
  We use the C++17 standard, and older compilers tend to fail on
  us. It seems that g++ 7.0 or higher is required nowadays (because of
  boost).  SimGrid compiles well with `clang` or `icc` too.
Python 3.
  SimGrid should build without Python. That is only needed by our regression test suite.
cmake (v3.5).
  ``ccmake`` provides a nicer graphical interface compared to ``cmake``.
  Press ``t`` in ``ccmake`` if you need to see absolutely all
  configuration options (e.g., if your Python installation is not standard).
boost mandatory components (at least v1.48, v1.59 recommended)
  - On Debian / Ubuntu: ``apt install libboost-dev``
  - On CentOS / Fedora: ``dnf install boost-devel``
  - On macOS with homebrew: ``brew install boost``
boost recommended components (optional).
  - boost-context may be used instead of our own fast context switching code which only works on amd64.
  - boost-stacktrace is used to get nice stacktraces on errors in SimGrid.
  - On Debian / Ubuntu: ``apt install libboost-context-dev libboost-stacktrace-dev``
python bindings (optional):
  - On Debian / Ubuntu: ``apt install pybind11-dev python3-dev``
Eigen3 (optional)
  - On Debian / Ubuntu: ``apt install libeigen3-dev``
  - On CentOS / Fedora: ``dnf install eigen3-devel``
  - On macOS with homebrew: ``brew install eigen``
  - Use EIGEN3_HINT to specify where it's installed if cmake doesn't find it automatically. Set EIGEN3_HINT=OFF to disable detection even if it could be found.
JSON (optional, for the DAG wfcommons loader)
  - On Debian / Ubuntu: ``apt install nlohmann-json3-dev``
  - Use nlohmann_json_HINT to specify where it's installed if cmake doesn't find it automatically.

For platform-specific details, please see below.

Getting the Sources
^^^^^^^^^^^^^^^^^^^

Grab the last **stable release** from `FramaGit
<https://framagit.org/simgrid/simgrid/-/releases>`_, and compile it as follows:

.. code-block:: console

   $ tar xf simgrid-3-XX.tar.gz
   $ cd simgrid-*
   $ cmake -DCMAKE_INSTALL_PREFIX=/opt/simgrid -GNinja .
   $ make
   $ make install

If you want to stay on the **bleeding edge**, get the current git version,
and recompile it as with stable archives. You may need some extra
dependencies.

.. code-block:: console

   $ git clone https://framagit.org/simgrid/simgrid.git
   $ cd simgrid
   $ cmake -DCMAKE_INSTALL_PREFIX=/opt/simgrid .
   $ make
   $ make install

.. _install_src_config:

Build Configuration
^^^^^^^^^^^^^^^^^^^

This section is about **compile-time options**, which are very
different from :ref:`run-time options <options>`. Compile-time options
fall into two categories. **SimGrid-specific options** define which part
of the framework to compile while **Generic options** are provided by
cmake itself.

.. warning::

   Our build system often gets mixed up if you change something on
   your machine after the build configuration.  For example, if
   SimGrid fails to detect your fortran compiler, it is not enough to
   install a fortran compiler. You also need to delete all Cmake
   files, such as ``CMakeCache.txt``. Since Cmake also generates some
   files in the tree, you may need to wipe out your complete tree and
   start with a fresh one when you install new dependencies.

   A better solution is to :ref:`build out of the source tree <install_cmake_outsrc>`.

Generic build-time options
""""""""""""""""""""""""""

These options specify, for example, the path to various system elements (Python
path, compiler to use, etc). In most case, CMake automatically discovers the
right value for these elements, but you can set them manually as needed.
Notably, such variables include ``CC`` and ``CXX``, defining the paths to the C
and C++ compilers; ``CFLAGS`` and ``CXXFLAGS`` specifying extra options to pass
to the C and C++ compilers; and ``PYTHON_EXECUTABLE`` specifying the path to the
Python executable.

The best way to discover the exact name of the option that you need to
change is to press ``t`` in the ``ccmake`` graphical interface, as all
options are shown (and documented) in the advanced mode.

Once you know their name, there are several ways to change the values of
build-time options. You can naturally use the ccmake graphical
interface for that, or you can use environment variables, or you can
prefer the ``-D`` flag of ``cmake``.

For example, you can change the compilers by issuing these commands to set some
environment variables before launching cmake:

.. code-block:: console

   $ export CC=gcc-5.1
   $ export CXX=g++-5.1

The same can be done by passing ``-D`` parameters to cmake, as follows.
Note that the dot at the end is mandatory (see :ref:`install_cmake_outsrc`).

.. code-block:: console

   $ cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ .

SimGrid compilation options
"""""""""""""""""""""""""""

Here is the list of all SimGrid-specific compile-time options (the
default choice is in upper case).

CMAKE_INSTALL_PREFIX (path)
  Where to install SimGrid (/opt/simgrid, /usr/local, or elsewhere).

enable_compile_optimizations (ON/off)
  Ask the compiler to produce efficient code. You probably want to
  leave this option activated, unless you plan to modify SimGrid itself:
  efficient code takes more time to compile, and appears mangled to some debuggers.

enable_compile_warnings (on/OFF)
  Ask the compiler to issue error messages whenever the source
  code is not perfectly clean. If you are a SimGrid developer, you
  have to activate this option to enforce the code quality. As a
  regular user, this option is of little use.

enable_debug (ON/off)
  Disabling this option discards all log messages of severity
  debug or below at compile time (see :ref:`outcome_logs`). The resulting
  code is marginaly faster than if you discard these messages at
  runtime, but it obviously becomes impossible to get any debug
  info from SimGrid when things go wrong.

enable_documentation (on/OFF)
  Generates the documentation pages. Building the documentation is not
  as easy as it used to be, and you should probably use the online
  version for now.

enable_lto (ON/off)
  Enables the *Link Time Optimization* in the C++ compiler.
  This feature really speeds up the code produced, but it is fragile
  with older gcc versions.

enable_maintainer_mode (on/OFF)
  (dev only) Regenerates the XML parsers whenever the DTD is modified (requires flex and flexml).

enable_mallocators (ON/off)
  Activates our internal memory caching mechanism. This produces faster
  code, but it may fool the debuggers.

enable_model-checking (ON/off)
  Activates the verification mode. This should not impact the performance of your simulations if you build it but don't use it,
  but you can still disable it to save some compilation time.

enable_ns3 (on/OFF)
  Activates the ns-3 bindings. See section :ref:`models_ns3`.

enable_smpi (ON/off)
  Allows one to run MPI code on top of SimGrid.

enable_testsuite_McMini (on/OFF)
  Adds several extra tests for the model checker module (targeting threaded applications).

enable_testsuite_smpi_MBI (on/OFF)
  Adds many extra tests for the model checker module (targeting MPI applications).

enable_testsuite_smpi_MPICH3 (on/OFF)
  Adds many extra tests for the MPI module.

minimal-bindings (on/OFF)
  Take as few optional dependencies as possible, to get minimal
  library bindings in Python.

NS3_HINT (empty by default)
  Alternative path into which ns-3 should be searched for.

EIGEN3_HINT (empty by default)
  Alternative path into which Eigen3 should be searched for.
  Providing the value OFF as an hint will disable the detection alltogether.

SIMGRID_PYTHON_LIBDIR (auto-detected)
  Where to install the Python module library. By default, it is set to the cmake Python3_SITEARCH variable if installing to /usr,
  and a modified version of that variable if installing to another path. Just force another value if the auto-detected default
  does not fit your setup.

SMPI_C_FLAGS, SMPI_CXX_FLAGS, SMPI_Fortran_FLAGS (string)
  Default compiler options to use in smpicc, smpicxx, or smpiff.
  This can be useful to set options like "-m32" or "-m64".

Reset the build configuration
"""""""""""""""""""""""""""""

To empty the CMake cache (either when you add a new library or when
things go seriously wrong), simply delete your ``CMakeCache.txt``. You
may also want to directly edit this file in some circumstances.

.. _install_cmake_outsrc:

Out of Tree Compilation
^^^^^^^^^^^^^^^^^^^^^^^

By default, the files produced during the compilation are placed in
the source directory. It is however often better to put them all in a
separate directory: cleaning the tree becomes as easy as removing this
directory, and you can have several such directories to test several
parameter sets or architectures.

For that, go to the directory where the files should be produced, and
invoke cmake (or ccmake) with the full path to the SimGrid source as
last argument.

.. code-block:: console

  $ mkdir build
  $ cd build
  $ cmake [options] ..
  $ make

Existing Compilation Targets
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

In most cases, compiling and installing SimGrid is enough:

.. code-block:: console

  $ make
  $ make install # try "sudo make install" if you don't have the permission to write

In addition, several compilation targets are provided in SimGrid. If
your system is well configured, the full list of targets is available
for completion when using the ``Tab`` key. Note that some of the
existing targets are not really for public consumption so don't worry
if some do not work for you.

- **make**: Build the core of SimGrid that gets installed, but not any example.
- **make examples**: Build the examples, which are needed by the tests.
- **make simgrid**: Build only the SimGrid library. Not any example nor the helper tools.
- **make s4u-comm-pingpong**: Build only this example (works for any example)
- **make python-bindings**: Build the Python bindings
- **make clean**: Clean the results of a previous compilation
- **make install**: Install the project (doc/ bin/ lib/ include/)
- **make dist**: Build a distribution archive (tar.gz)
- **make distcheck**: Check the dist (make + make dist + tests on the distribution)
- **make documentation**: Create SimGrid documentation

If you want to see what is really happening, try adding ``VERBOSE=1`` to
your compilation requests:

.. code-block:: console

  $ make VERBOSE=1

.. _install_src_test:

Testing your build
^^^^^^^^^^^^^^^^^^

Once everything is built, you may want to test the result. SimGrid
comes with an extensive set of regression tests (as described in the
@ref inside_tests "insider manual"). The tests are not built by
default, so you first have to build them with ``make tests``. You can
then run them with ``ctest``, that comes with CMake.  We run them
every commit and the results are on `our Jenkins <https://ci.inria.fr/simgrid/>`_.

.. code-block:: console

  $ make tests                # Build the tests
  $ ctest                     # Launch all tests
  $ ctest -R s4u              # Launch only the tests whose names match the string "s4u"
  $ ctest -j4                 # Launch all tests in parallel, at most 4 concurrent jobs
  $ ctest --verbose           # Display all details on what's going on
  $ ctest --output-on-failure # Only get verbose for the tests that fail

  $ ctest -R s4u -j4 --output-on-failure # You changed S4U and want to check that you  \
                                         # didn't break anything, huh?                 \
                                         # That's fine, I do so all the time myself.

.. _install_cmake_mac:

macOS-specific instructions
^^^^^^^^^^^^^^^^^^^^^^^^^^^

SimGrid compiles like a charm with clang (version 3.0 or higher) on macOS:

.. code-block:: console

  $ cmake -DCMAKE_C_COMPILER=/path/to/clang -DCMAKE_CXX_COMPILER=/path/to/clang++ .
  $ make


Troubleshooting your macOS build.

CMake Error: Parse error in cache file build_dir/CMakeCache.txt. Offending entry: /SDKs/MacOSX10.8.sdk
  This was reported with the XCode version of clang 4.1. The work
  around is to edit the ``CMakeCache.txt`` file directly, to change
  the following entry:

  ``CMAKE_OSX_SYSROOT:PATH=/Applications/XCode.app/Contents/Developer/Platforms/MacOSX.platform/Developer``

  You can safely ignore the warning about "-pthread" not being used, if it appears.

/usr/include does not seem to exist
  This directory does not exist by default on modern macOS versions,
  and you may need to create it with ``xcode-select -install``

.. _install_cmake_windows:

Windows-specific instructions
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The best solution to get SimGrid working on windows is to install the
Ubuntu subsystem of Windows 10. All of SimGrid 
works in this setting. Native builds never really worked, and they are
disabled starting with SimGrid v3.33.

Python-specific instructions
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Once you have the Python development headers installed as well as a
recent version of the `pybind11 <https://pybind11.readthedocs.io/en/stable/>`_
module (version at least 2.4), recompiling the Python bindings from
the source should be as easy as:

.. code-block:: console

  # cd simgrid-source-tree
  $ python setup.py build install

Starting with SimGrid 3.13, it should even be possible to install
simgrid without downloading the source with pip:

.. code-block:: console

  $ pip install simgrid

If you installed SimGrid to a non-standard directory (such as ``/opt/simgrid`` as advised earlier), you should tell python where
to find the libraries as follows (notice the elements suffixed to the configured prefix).

.. code-block:: console

  $ PYTHONPATH="/opt/simgrid/lib/python3/dist-packages" LD_LIBRARY_PATH="/opt/simgrid/lib" python your_script.py

You can add those variables to your bash profile to not specify it each time by adding these lines to your ``~/.profile``:

.. code-block:: console

  export PYTHONPATH="$PYTHONPATH:/opt/simgrid/lib/python3/dist-packages"
  export LD_LIBRARY_PATH="$PYTHONPATH:/opt/simgrid/lib"
