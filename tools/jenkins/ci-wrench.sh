#! /bin/sh

# Test this script locally as follows (rerun `docker pull simgrid/unstable` to get a fresh version).
# cd (simgrid)/tools/jenkins
# docker run -it --rm --volume `pwd`:/source simgrid/unstable /source/ci-wrench.sh

set -ex

export CXX="g++"
export CC="gcc"
export SUDO=""

echo "XXXXXXXXXXXXXXXX Install APT dependencies"

$SUDO apt-get update
$SUDO apt-get -y install cmake gcc g++ git
$SUDO apt-get -y install unzip doxygen wget
$SUDO apt-get -y install libboost-all-dev libpugixml-dev nlohmann-json3-dev libgtest-dev

echo "XXXXXXXXXXXXXXX Install SimGrid FSMod"
wget https://github.com/simgrid/file-system-module/archive/refs/tags/v0.2.tar.gz && tar -xf v0.2.tar.gz
(mkdir file-system-module-0.2/build && cd file-system-module-0.2/build && cmake .. && make -j$(nproc) && $SYDO make install && cd ../.. && rm -rf file-system-module-0.2) || exit 1


echo "XXXXXXXXXXXXXXXX build and test wrench (git version)"
# install WRENCH from their upstream git into the wrench.git directory
rm -rf wrench.git && git clone --depth 1 --branch simgrid-external-project-ci https://github.com/wrench-project/wrench.git wrench.git
(mkdir wrench.git/build && cd wrench.git/build && cmake -DSIMGRID_INSTALL_PATH=/usr/ .. && make -j$(nproc) unit_tests && ./unit_tests && cd ../.. && rm -rf wrench.git) || exit 1



