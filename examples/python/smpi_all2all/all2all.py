# Copyright (c) 2010-2024. The SimGrid Team. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the license (GNU LGPL) which comes with this package.

import sys
from argparse import ArgumentParser

import numpy as np

import simgrid
from simgrid import Engine, this_actor


def create_parser() -> ArgumentParser:
    parser = ArgumentParser()
    parser.add_argument(
        '--platform',
        type=str,
        required=True,
        help='path to the platform description'
    )
    return parser


def pinger():
    simgrid.smpi.MPI.Init()
    MPI_COMM_WORLD = simgrid.smpi.MPI.Comm.WORLD
    size = int(0)
    size = simgrid.smpi.MPI.Comm.size(MPI_COMM_WORLD)
    this_actor.info(f"ping {size}")
    rank = 0
    rank = simgrid.smpi.MPI.Comm.rank(MPI_COMM_WORLD)
    this_actor.info(f"ping {rank}")
    input_size = 10
    half_size = input_size // 2
    np.random.seed(5)
    np_in = np.random.randint(100, size=input_size, dtype=np.int32)
    np_out = np.zeros(input_size, dtype=np.int32)
    simgrid.smpi.MPI.Alltoall(np_in, half_size, simgrid.smpi.MPI.Datatype.INT, np_out, half_size, simgrid.smpi.MPI.Datatype.INT, MPI_COMM_WORLD)
    this_actor.info(f"input first half {np_in[:half_size]}")
    this_actor.info(f"input second half {np_in[half_size:]}")

    this_actor.info(f"output first half {np_out[:half_size]}")
    this_actor.info(f"output second half {np_out[half_size:]}")
    simgrid.smpi.MPI.Finalize()

def main():
    settings = create_parser().parse_known_args()[0]
    e = Engine(sys.argv)
    simgrid.smpi.init()
    e.load_platform(settings.platform)


    simgrid.smpi.app_instance_start("pinger", pinger, [e.host_by_name("Jupiter"), e.host_by_name("Tremblay")])

    e.run()

    this_actor.info(f"Total simulation time: {e.clock:.3f}")


if __name__ == "__main__":
    main()
