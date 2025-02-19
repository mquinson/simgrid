#!/usr/bin/env sh

die() {
    echo "$@"
    exit 1
}

[ -n "$WORKSPACE" ] || die "No WORKSPACE"
[ -d "$WORKSPACE" ] || die "WORKSPACE ($WORKSPACE) does not exist"

echo "XXXX Cleanup previous attempts. Remaining content of /tmp:"
rm -f /tmp/cc*
rm -f /tmp/simgrid-mc-*
rm -f /tmp/*.so
rm -f /tmp/*.so.*
rm -f /tmp/simgrid-java*
ls /tmp
df -h
echo "XXXX Let's go"

set -e

### Check the node installation

pkg_check() {
  for pkg
  do
    if command -v "$pkg"
    then
       echo "$pkg is installed. Good."
    else
       die "please install $pkg before proceeding"
    fi
  done
}

pkg_check valgrind pcregrep

### Cleanup previous runs

do_cleanup() {
  for d
  do
    if [ -d "$d" ]
    then
      rm -rf "$d" || die "Could not remove $d"
    fi
    mkdir "$d" || die "Could not create $d"
  done
  find "$WORKSPACE" -name "memcheck_test_*.memcheck" -exec rm {} \;
}

do_cleanup "$WORKSPACE/build" "$WORKSPACE/memcheck" "$WORKSPACE/generatedJUnitFiles"

NUMPROC="$(nproc)" || NUMPROC=1

cd "$WORKSPACE"/build

### Proceed with the tests
ctest -D ExperimentalStart || true

cmake -Denable_documentation=OFF -Denable_python=OFF \
      -Denable_compile_optimizations=OFF -Denable_compile_warnings=ON \
      -Denable_mallocators=OFF \
      -Denable_smpi=ON -Denable_testsuite_smpi_MPICH3=OFF -Denable_testsuite_McMini=OFF -Denable_model-checking=OFF \
      -Denable_ns3=ON \
      -Denable_memcheck_xml=ON -DLTO_EXTRA_FLAG="auto" "$WORKSPACE"


make -j$NUMPROC tests
ctest --no-compress-output -D ExperimentalTest -j$NUMPROC || true

cd "$WORKSPACE"/build
if [ -f Testing/TAG ] ; then
   find "$WORKSPACE" -iname "*.memcheck" -exec mv {} "$WORKSPACE"/memcheck \;
   #remove all "empty" files
   grep -r -L "error>" "$WORKSPACE"/memcheck | xargs rm -f
   mv Testing/"$(head -n 1 < Testing/TAG)"/Test.xml  "$WORKSPACE"/DynamicAnalysis.xml
fi
