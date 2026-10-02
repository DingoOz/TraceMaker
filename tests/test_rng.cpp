#include <catch2/catch_test_macros.hpp>

#include <set>

#include "core/rng.hpp"

using tmk::U32x2;
using tmk::U32x4;

// Known-answer tests from the Random123 distribution (kat_vectors, philox4x32 10 rounds).
TEST_CASE("Philox4x32-10 matches the Random123 known-answer vectors", "[core][rng]") {
  CHECK(tmk::philox4x32_10(U32x4{{0, 0, 0, 0}}, U32x2{{0, 0}}) ==
        U32x4{{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u}});
  CHECK(tmk::philox4x32_10(U32x4{{0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}},
                          U32x2{{0xffffffffu, 0xffffffffu}}) ==
        U32x4{{0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu}});
  CHECK(tmk::philox4x32_10(U32x4{{0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u}},
                          U32x2{{0xa4093822u, 0x299f31d0u}}) ==
        U32x4{{0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u}});
}

TEST_CASE("RNG streams are pure functions of (seed, stage, item, n)", "[core][rng]") {
  const tmk::RngStream a(42, 3, 7), b(42, 3, 7);
  for (std::uint64_t n = 0; n < 100; ++n) REQUIRE(a.u64(n) == b.u64(n));

  // Different seeds, stages or items give different streams.
  std::set<std::uint64_t> firsts;
  for (std::uint64_t seed = 0; seed < 8; ++seed)
    for (std::uint32_t stage = 0; stage < 8; ++stage)
      for (std::uint64_t item = 0; item < 8; ++item) firsts.insert(tmk::RngStream(seed, stage, item).u64(0));
  CHECK(firsts.size() == 512);

  for (std::uint64_t n = 0; n < 1000; ++n) {
    const double u = a.uniform(n);
    REQUIRE(u >= 0.0);
    REQUIRE(u < 1.0);
  }
}
