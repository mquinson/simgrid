#!/usr/bin/env sh

# This script is used by various build projects on Jenkins

case "$JENKINS_HOME" in
*-qualif)
  echo "Build skipped on $JENKINS_HOME."
  exit 0
  ;;
esac

# See https://ci.inria.fr/simgrid/job/SimGrid/configure
# See https://ci.inria.fr/simgrid/job/Simgrid-Windows/configure

# ensure that the locales are set, so that perl keeps its nerves
export LC_ALL=C

echo "XXXX Cleanup previous attempts. Remaining content of /tmp:"
rm -f /tmp/cc*
rm -f /tmp/simgrid-mc-*
rm -f /tmp/*.so
rm -f /tmp/*.so.*
rm -rf /tmp/simgrid-java-*
ls /tmp
df -h
echo "XXXX Let's go"

set -e

# usage: die status message...
die () {
  status=${1:-1}
  shift
  [ $# -gt 0 ] || set -- "Error - Halting"
  echo "$@" >&2
  exit "$status"
}

# Get an ON/OFF string from a command:
onoff() {
  if "$@" > /dev/null ; then
    echo ON
  else
    echo OFF
  fi
}

if type lsb_release >/dev/null 2>&1; then # recent versions of Debian/Ubuntu
    # linuxbase.org
    os=$(lsb_release -si)
    ver="$(lsb_release -sr) ($(lsb_release -sc))"
elif [ -f /etc/lsb-release ]; then # For some versions of Debian/Ubuntu without lsb_release command
    . /etc/lsb-release
    os=$DISTRIB_ID
    ver=$DISTRIB_RELEASE
elif [ -f /etc/debian_version ]; then # Older Debian/Ubuntu/etc.
    os=Debian
    ver=$(cat /etc/debian_version)
elif [ -f /etc/redhat-release ]; then #RH, Fedora, Centos
    read -r os ver < /etc/redhat-release
elif [ -f /usr/bin/sw_vers ]; then #osx
    os=$(sw_vers -productName)
    ver=$(sw_vers -productVersion)
elif [ -f /bin/freebsd-version ]; then #freebsd
    os=$(uname -s)
    ver=$(freebsd-version -u)
elif [ -f /etc/release ]; then #openindiana
    read -r os ver < /etc/release
elif [ -f /etc/os-release ]; then # freedesktop.org and systemd, put last as usually missing useful info
    . /etc/os-release
    os=$NAME
    ver=$VERSION_ID
else
    # Fall back to uname, e.g. "Linux <version>", also works for BSD, etc.
    echo "fallback as OS name not found"
    os=$(uname -s)
    ver=$(uname -r)
fi

# Are we running on wsl ?
if [ -f /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe ]; then
    #To identify the windows underneath the linux
    PATH="/mnt/c/Windows/System32/WindowsPowerShell/v1.0/:$PATH"
    major=$(powershell.exe -command "[environment]::OSVersion.Version.Major" | sed 's/\r//g')
    build=$(powershell.exe -command "[environment]::OSVersion.Version.Build"| sed 's/\r//g')
    ver="$major v$build - WSL $os $ver"
    os=Windows
fi

case $(uname -m) in
x86_64)
    bits="64 bits"
    ;;
i*86)
    bits="32 bits"
    ;;
*)
    bits=""
    ;;
esac
echo "OS Version : '$os' $ver, $bits"


build_mode="$1"
echo "Build mode $build_mode on $(uname -np)" >&2
case "$build_mode" in
  "Debug")
      INSTALL="$HOME/simgrid_install"
  ;;

  "ModelChecker")
      INSTALL="$HOME/mc_simgrid_install"
  ;;

  "Release")
      INSTALL="$HOME/simgrid_install_release"
      rm -rf "$HOME/mc_simgrid_install" # FIXME we can remove it once it ran once after the move from MC build to release builds
  ;;

  "DynamicAnalysis")
      INSTALL=""
  ;;

  *)
    die 1 "Unknown build_mode $build_mode"
  ;;
esac

if [ "$2" = "" ]; then
  branch_name="unknown"
else
  branch_name="$2"
fi
echo "Branch built is $branch_name"

NUMBER_OF_PROCESSORS="$(nproc)" || NUMBER_OF_PROCESSORS=1
GENERATOR="Unix Makefiles"
BUILDER=make
VERBOSE_BUILD="VERBOSE=1"
if which ninja 2>/dev/null >/dev/null ; then
  GENERATOR=Ninja
  BUILDER=ninja
  VERBOSE_BUILD="-v"
fi

ulimit -c 0 || true

echo "XX"
echo "XX Get out of the tree"
echo "XX"
if [ -d "$WORKSPACE"/build ]
then
  # Windows cannot remove the directory if it's still used by the previous build
  rm -rf "$WORKSPACE"/build || sleep 10 && rm -rf "$WORKSPACE"/build || sleep 10 && rm -rf "$WORKSPACE"/build
fi
mkdir "$WORKSPACE"/build
cd "$WORKSPACE"/build

have_NS3="no"
if [ "$os" = "Debian" ] ; then
  if dpkg --compare-versions "$(dpkg-query -f '${Version}' -W libns3-dev)" ge 3.28; then
    have_NS3="yes"
  fi
fi
if [ "$os" = "nixos" ] ; then
  have_NS3="yes"
fi
echo "XX have_NS3: ${have_NS3}"

SIMGRID_PYTHON_LIBDIR=""
if [ "$os" = "nixos" ] ; then
  SIMGRID_PYTHON_LIBDIR="/home/ci/simgrid_install/lib64"
fi
echo "XX SIMGRID_PYTHON_LIBDIR: ${SIMGRID_PYTHON_LIBDIR}"

# This is for Windows:
PATH="$WORKSPACE/build/lib:$PATH"

echo "XX"
echo "XX Build the archive out of the tree"
echo "XX   pwd: $(pwd)"
echo "XX"

cmake -G"$GENERATOR" -Denable_documentation=OFF "$WORKSPACE"
${BUILDER} dist -j $NUMBER_OF_PROCESSORS
SIMGRID_VERSION=$(cat VERSION)

echo "XX"
echo "XX Open the resulting archive"
echo "XX"
gunzip "${SIMGRID_VERSION}".tar.gz
tar xf "${SIMGRID_VERSION}".tar
mkdir "${WORKSPACE}"/build/"${SIMGRID_VERSION}"/build
cd "${WORKSPACE}"/build/"${SIMGRID_VERSION}"/build
SRCFOLDER="${WORKSPACE}/build/${SIMGRID_VERSION}"

echo "XX"
echo "XX Configure and build SimGrid"
echo "XX   pwd: $(pwd)"
echo "XX"
set -x

if [ "$os" = "CentOS" ]; then
    if [ "$(ld -v | cut -d\  -f4 | cut -c1-4)" = "2.30" ]; then
        echo "Temporary disable LTO, believed to be broken on this system."
        MAY_DISABLE_LTO=-Denable_lto=OFF
    else
        MAY_DISABLE_LTO=
    fi
fi

MAYBE_MCMINI=-Denable_testsuite_McMini=ON
if [ $NODE_NAME = "armv8" ]; then
    echo "disable LTO, believed to be too heavy for this particular system"
    MAY_DISABLE_LTO=-Denable_lto=OFF
    MAYBE_MCMINI=-Denable_testsuite_McMini=OFF
fi

cmake -G"$GENERATOR" ${INSTALL:+-DCMAKE_INSTALL_PREFIX=$INSTALL} \
  -Denable_debug=ON -Denable_documentation=OFF -Denable_coverage=OFF \
  -Denable_model-checking=ON \
  -Denable_testsuite_smpi_MBI=OFF ${MAYBE_MCMINI} -Denable_testsuite_smpi_MPICH3=ON \
  -Denable_compile_optimizations=$(onoff test "$build_mode" != "DynamicAnalysis" -a "$build_mode" != "Debug") \
  -Denable_mallocators=$(onoff test "$build_mode" != "DynamicAnalysis") \
  -Denable_memcheck=$(onoff test "$build_mode" = "DynamicAnalysis") \
  -Denable_compile_warnings=ON -Denable_smpi=ON \
  -Denable_lib_in_jar=OFF \
  -Denable_ns3=$(onoff test "$have_NS3" = "yes") \
  -DSIMGRID_PYTHON_LIBDIR=${SIMGRID_PYTHON_LIBDIR} \
  ${MAY_DISABLE_LTO} -DLTO_EXTRA_FLAG="auto" \
  -DCMAKE_DISABLE_SOURCE_CHANGES=ON \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  "$SRCFOLDER"

${BUILDER} -j $NUMBER_OF_PROCESSORS ${VERBOSE_BUILD} tests

echo "XX"
echo "XX Run the tests"
echo "XX   pwd: "$(pwd)
echo "XX"

ctest -T test --output-on-failure --no-compress-output -j $NUMBER_OF_PROCESSORS || true

if test -n "$INSTALL" && [ "${branch_name}" = "origin/master" ] ; then
  echo "XX"
  echo "XX Test done. Install everything since it's a regular build, not on a Windows."
  echo "XX"

  rm -rf "$INSTALL"

  ${BUILDER} install
fi

echo "XX"
echo "XX Done. Return the results to cmake"
echo "XX"
