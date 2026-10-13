// Fixtures come from tools/generators/render_fixtures.py; the expected pixels below are the ones
// that script writes.

#include "engine/render/image.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/vfs/vfs.hpp"

namespace {

const std::filesystem::path kImages =
    std::filesystem::path{VNMAKI_FIXTURES_DIR} / "render" / "images";

std::vector<std::byte> read_fixture(const std::string& name) {
  std::ifstream file{kImages / name, std::ios::binary};
  REQUIRE(file);
  std::vector<char> chars{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes;
  bytes.reserve(chars.size());
  for (const char c : chars) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

/// The RGBA bytes of pixel (x, y).
std::vector<int> pixel_at(const vnmaki::Image& image, int x, int y) {
  constexpr std::size_t kChannels = 4;
  const std::size_t offset =
      ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.size.width)) +
       static_cast<std::size_t>(x)) *
      kChannels;
  return {image.pixels.at(offset), image.pixels.at(offset + 1), image.pixels.at(offset + 2),
          image.pixels.at(offset + 3)};
}

bool near(const std::vector<int>& actual, const std::vector<int>& expected, int tolerance) {
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (std::abs(actual.at(i) - expected.at(i)) > tolerance) {
      return false;
    }
  }
  return true;
}

vnmaki::Vfs fixture_vfs() {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(kImages)));
  return vfs;
}

}  // namespace

TEST_CASE("image: a PNG decodes to RGBA with premultiplied alpha") {
  const vnmaki::Image image = vnmaki::decode_image(read_fixture("rgba_2x2.png"));

  CHECK(image.size == vnmaki::Size{.width = 2, .height = 2});
  const std::vector<std::uint8_t> expected{
      255, 0,   0,   255,  // opaque red
      0,   255, 0,   255,  // opaque green
      0,   0,   128, 128,  // half-transparent blue: colour scaled by alpha
      0,   0,   0,   0,    // transparent white: colour gone
  };
  CHECK(image.pixels == expected);
}

TEST_CASE("image: a PNG without alpha decodes as opaque") {
  const vnmaki::Image image = vnmaki::decode_image(read_fixture("rgb_3x1.png"));

  CHECK(image.size == vnmaki::Size{.width = 3, .height = 1});
  const std::vector<std::uint8_t> expected{10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255};
  CHECK(image.pixels == expected);
}

TEST_CASE("image: a PNG with an empty IDAT chunk decodes") {
  // Regression (found by fuzz_image): stb_image used to trip UBSan on the zero-length chunk.
  const vnmaki::Image image = vnmaki::decode_image(read_fixture("empty_idat_rgb_3x1.png"));

  const std::vector<std::uint8_t> expected{10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255};
  CHECK(image.pixels == expected);
}

TEST_CASE("image: a JPEG decodes to opaque RGBA") {
  const vnmaki::Image image = vnmaki::decode_image(read_fixture("blocks_16x8.jpg"));

  REQUIRE(image.size == vnmaki::Size{.width = 16, .height = 8});
  // JPEG is lossy (YCbCr rounding), so colours match within a small tolerance.
  constexpr int kTolerance = 3;
  const std::vector<int> orange{240, 120, 32, 255};
  const std::vector<int> teal{32, 160, 160, 255};
  CHECK(near(pixel_at(image, 0, 0), orange, kTolerance));
  CHECK(near(pixel_at(image, 7, 7), orange, kTolerance));
  CHECK(near(pixel_at(image, 8, 0), teal, kTolerance));
  CHECK(near(pixel_at(image, 15, 7), teal, kTolerance));
}

TEST_CASE("image: data that is not a whole PNG or JPEG is an ImageError") {
  for (const char* name : {"not_an_image.png", "truncated.png", "empty.png"}) {
    CAPTURE(name);
    CHECK_THROWS_AS(static_cast<void>(vnmaki::decode_image(read_fixture(name))),
                    vnmaki::ImageError);
  }
}

TEST_CASE("image: an image over the decoder's size limits is refused from its header") {
  CHECK(vnmaki::kMaxImageDimension == 16384);
  CHECK(vnmaki::kMaxImagePixels == 67108864);
  // 16385x1: one pixel wider than allowed.
  CHECK_THROWS_WITH_AS(static_cast<void>(vnmaki::decode_image(read_fixture("too_wide.png"))),
                       doctest::Contains("too large"), vnmaki::ImageError);
  // 8193x8193 has no pixel data at all; only the size check can name it too large.
  CHECK_THROWS_WITH_AS(static_cast<void>(vnmaki::decode_image(read_fixture("too_many_pixels.png"))),
                       doctest::Contains("too large"), vnmaki::ImageError);
}

TEST_CASE("image: load_image reads through the VFS and decodes on the job system") {
  const vnmaki::Vfs vfs = fixture_vfs();
  constexpr std::size_t kWorkers = 2;
  for (const std::size_t workers : {std::size_t{0}, kWorkers}) {
    CAPTURE(workers);
    vnmaki::JobSystem jobs{workers};

    vnmaki::JobHandle<vnmaki::Image> handle = vnmaki::load_image(vfs, jobs, "rgb_3x1.png");
    const vnmaki::Image image = handle.get();

    CHECK(image.size == vnmaki::Size{.width = 3, .height = 1});
    CHECK(image.pixels.at(3) == 255);
  }
}

TEST_CASE("image: load_image reports a missing or undecodable file through the handle") {
  const vnmaki::Vfs vfs = fixture_vfs();
  vnmaki::JobSystem jobs{1};

  vnmaki::JobHandle<vnmaki::Image> missing = vnmaki::load_image(vfs, jobs, "no/such.png");
  vnmaki::JobHandle<vnmaki::Image> broken = vnmaki::load_image(vfs, jobs, "truncated.png");

  CHECK_THROWS_WITH_AS(static_cast<void>(missing.get()), doctest::Contains("no/such.png"),
                       vnmaki::ImageError);
  CHECK_THROWS_WITH_AS(static_cast<void>(broken.get()), doctest::Contains("truncated.png"),
                       vnmaki::ImageError);
}
