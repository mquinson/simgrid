/* Copyright (c) 2019-2025. The SimGrid Team. All rights reserved.               */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/3rd-party/catch.hpp"
#include "xbt/log.h"
#include "xbt/random.hpp"
#include <cmath>
#include <random>

#define EpsilonApprox(a) Catch::Matchers::WithinAbs((a), 100 * std::numeric_limits<double>::epsilon())

TEST_CASE("xbt::random: Random Number Generation")
{
  SECTION("Using XBT_RNG_xbt")
  {
    simgrid::xbt::random::set_implem_xbt();
    simgrid::xbt::random::set_mersenne_seed(12345);
    REQUIRE_THAT(simgrid::xbt::random::exponential(25), EpsilonApprox(0.00291934351538427348));
    REQUIRE(simgrid::xbt::random::uniform_int(1, 6) == 4);
    REQUIRE_THAT(simgrid::xbt::random::uniform_real(0, 1), EpsilonApprox(0.31637556043369124970));
    REQUIRE_THAT(simgrid::xbt::random::normal(0, 2), EpsilonApprox(1.62746784745133976635));

    constexpr int imin = std::numeric_limits<int>::min();
    constexpr int imax = std::numeric_limits<int>::max();
    REQUIRE(simgrid::xbt::random::uniform_int(0, 0) == 0);
    REQUIRE(simgrid::xbt::random::uniform_int(imin, imin) == imin);
    REQUIRE(simgrid::xbt::random::uniform_int(imax, imax) == imax);

    REQUIRE(simgrid::xbt::random::uniform_int(-6, -1) == -3);
    REQUIRE(simgrid::xbt::random::uniform_int(-10, 10) == 7);
    REQUIRE(simgrid::xbt::random::uniform_int(imin, 2) == -163525263);
    REQUIRE(simgrid::xbt::random::uniform_int(-2, imax) == 1605979225);
    REQUIRE(simgrid::xbt::random::uniform_int(imin, imax) == 659577591);
  }

  SECTION("Using XBT_RNG_std")
  {
    std::mt19937 gen;
    gen.seed(12345);

    simgrid::xbt::random::set_implem_std();
    simgrid::xbt::random::set_mersenne_seed(12345);

    std::exponential_distribution distA(25.0);
    std::uniform_int_distribution distB(1, 6);
    std::uniform_real_distribution distC(0.0, 1.0);
    std::normal_distribution distD(0.0, 2.0);

    REQUIRE_THAT(simgrid::xbt::random::exponential(25), EpsilonApprox(distA(gen)));
    REQUIRE(simgrid::xbt::random::uniform_int(1, 6) == distB(gen));
    REQUIRE_THAT(simgrid::xbt::random::uniform_real(0, 1), EpsilonApprox(distC(gen)));
    REQUIRE_THAT(simgrid::xbt::random::normal(0, 2), EpsilonApprox(distD(gen)));
  }

  SECTION("XBT_RNG_std write to a file")
  {
    simgrid::xbt::random::set_implem_std();
    simgrid::xbt::random::set_mersenne_seed(12345);

    simgrid::xbt::random::exponential(25);
    bool writtenA = simgrid::xbt::random::write_mersenne_state("rdm_state_tmp.txt");
    double resB = simgrid::xbt::random::uniform_real(10, 20);
    double resC = simgrid::xbt::random::normal(0, 2);
    bool writtenB = simgrid::xbt::random::read_mersenne_state("rdm_state_tmp.txt");
    REQUIRE(writtenA);
    REQUIRE(writtenB);
    REQUIRE_THAT(simgrid::xbt::random::uniform_real(10, 20), EpsilonApprox(resB));
    REQUIRE_THAT(simgrid::xbt::random::normal(0, 2), EpsilonApprox(resC));
    if (writtenB) {
      std::remove("rdm_state_tmp.txt");
    }
  }
}
