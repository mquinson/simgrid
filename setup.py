# Copyright (c) 2019-2024. The SimGrid Team. All rights reserved.

# This program is free software; you can redistribute it and/or modify it
# under the terms of the license (GNU LGPL-2.1-only) which comes with this package.

# python3 setup.py sdist # Build a source distrib (building binary distribs is complex on linux)

# twine upload --repository-url https://test.pypi.org/legacy/ dist/simgrid-*.tar.gz # Upload to test
# pip3 install --user --index-url https://test.pypi.org/simple  simgrid

# Once it works, upload to the real infra.  /!\ you cannot modify a file once uploaded
# twine upload dist/simgrid-*.tar.gz

import os
import platform
import re
import subprocess
import sys
from distutils.version import LooseVersion

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ENABLE_SMPI = False

if "--enable-smpi" in sys.argv:
    ENABLE_SMPI = True
    sys.argv.remove("--enable-smpi")


class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=''):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext):
    def run(self):
        try:
            out = subprocess.check_output(['cmake', '--version'])
        except OSError:
            raise RuntimeError(
                "CMake must be installed to build python bindings of SimGrid")

        if not os.path.exists("MANIFEST.in"):
            raise RuntimeError(
                "Please generate a MANIFEST.in file (configure SimGrid, and copy it here if you build out of tree)")

        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext):
        from pybind11 import get_cmake_dir
        extdir = os.path.abspath(os.path.dirname(
            self.get_ext_fullpath(ext.name)))
        enable_smpi = 'ON' if ENABLE_SMPI else 'OFF'
        cmake_args = ['-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=' + extdir,
                      '-DPYTHON_EXECUTABLE=' + sys.executable,
                      '-Denable_smpi=' + enable_smpi,
                      '-Denable_java=OFF',
                      '-Denable_python=ON',
                      '-Dminimal-bindings=ON',
                      '-Dpybind11_DIR=' + get_cmake_dir()
                      ]

        cfg = 'Debug' if self.debug else 'Release'
        build_args = ['--config', cfg]

        cmake_args += ['-DCMAKE_BUILD_TYPE=' + cfg]
        build_args += ['--', '-j4']

        env = os.environ.copy()
        env['CXXFLAGS'] = '{} -DVERSION_INFO=\\"{}\\"'.format(env.get('CXXFLAGS', ''),
                                                              self.distribution.get_version())
        env['LDFLAGS'] = '{} -L{}'.format(env.get('LDFLAGS', ''), extdir)

        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)
        subprocess.check_call(['cmake', ext.sourcedir] +
                              cmake_args, cwd=self.build_temp, env=env)
        subprocess.check_call(['cmake', '--build', '.'] +
                              build_args, cwd=self.build_temp, env=env)


setup(
    name='simgrid',
    version='3.36.1',
    author='Da SimGrid Team',
    author_email='simgrid-community@inria.fr',
    description='Toolkit for scalable simulation of distributed applications',
    long_description=("SimGrid is a scientific instrument to study the behavior of "
                      "large-scale distributed systems such as Grids, Clouds, HPC or P2P "
                      "systems. It can be used to evaluate heuristics, prototype applications "
                      "or even assess legacy MPI applications.\n\n"
                      "This package contains a native library. Please install cmake, boost, pybind11 and a "
                      "C++ compiler before using pip3. On Debian/Ubuntu, this is as easy as\n"
                      "sudo apt install cmake libboost-dev pybind11-dev g++ gcc"),
    ext_modules=[CMakeExtension('simgrid')],
    cmdclass=dict(build_ext=CMakeBuild),
    install_requires=['pybind11>=2.4'],
    setup_requires=['pybind11>=2.4'],
    zip_safe=False,
    classifiers=[
        "Development Status :: 4 - Beta",
        "Environment :: Console",
        "Intended Audience :: Education",
        "Intended Audience :: Developers",
        "Intended Audience :: Science/Research",
        "Intended Audience :: System Administrators",
        "License :: OSI Approved :: GNU Lesser General Public License v2 (LGPLv2)",
        "Operating System :: POSIX",
        "Operating System :: MacOS",
        "Programming Language :: Python :: 3",
        "Programming Language :: C++",
        "Programming Language :: C",
        "Programming Language :: Fortran",
        "Programming Language :: Java",
        "Topic :: System :: Distributed Computing",
        "Topic :: System :: Systems Administration",
    ],
    url="https://simgrid.org",
    project_urls={
        'Tracker': 'https://framagit.org/simgrid/simgrid/issues/',
        'Source':  'https://framagit.org/simgrid/simgrid/',
        'Documentation': 'https://simgrid.org/doc/latest/',
    },
)
