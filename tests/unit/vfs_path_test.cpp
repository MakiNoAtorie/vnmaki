#include <optional>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "engine/vfs/path.hpp"

using vnmaki::normalize_virtual_path;

TEST_CASE("virtual path: backslashes, empty and dot segments normalize to forward slashes") {
  CHECK(normalize_virtual_path("images\\bg\\\\.\\park.png") ==
        std::optional<std::string>{"images/bg/park.png"});
}

TEST_CASE("virtual path: '..' removes the previous segment") {
  CHECK(normalize_virtual_path("a/b/../c/./d/..") == std::optional<std::string>{"a/c"});
}

TEST_CASE("virtual path: '..' that would leave the root is rejected") {
  CHECK_FALSE(normalize_virtual_path("..").has_value());
  CHECK_FALSE(normalize_virtual_path("../secret.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("a/../../secret.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("a\\..\\..\\secret.txt").has_value());
}

TEST_CASE("virtual path: absolute paths are rejected") {
  CHECK_FALSE(normalize_virtual_path("/etc/passwd").has_value());
  CHECK_FALSE(normalize_virtual_path("\\Windows\\win.ini").has_value());
  CHECK_FALSE(normalize_virtual_path("C:/Windows/win.ini").has_value());
  CHECK_FALSE(normalize_virtual_path("C:win.ini").has_value());
  CHECK_FALSE(normalize_virtual_path("\\\\server\\share\\file").has_value());
}

TEST_CASE("virtual path: characters some desktop file systems forbid are rejected") {
  using namespace std::string_view_literals;
  CHECK_FALSE(normalize_virtual_path("a\0b.txt"sv).has_value());
  CHECK_FALSE(normalize_virtual_path("tab\there.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("what?.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("glob*.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("<tag>.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("pipe|.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("quote\".txt").has_value());
}

TEST_CASE("virtual path: segments Windows would alias or treat as devices are rejected") {
  CHECK_FALSE(normalize_virtual_path("dir./file.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("file.txt ").has_value());
  CHECK_FALSE(normalize_virtual_path("...").has_value());
  CHECK_FALSE(normalize_virtual_path("sounds/con").has_value());
  CHECK_FALSE(normalize_virtual_path("NUL.png").has_value());
  CHECK_FALSE(normalize_virtual_path("data/com1.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("Lpt9").has_value());
  CHECK(normalize_virtual_path("console.txt") == std::optional<std::string>{"console.txt"});
  CHECK(normalize_virtual_path("com10.txt") == std::optional<std::string>{"com10.txt"});
  CHECK(normalize_virtual_path(".hidden") == std::optional<std::string>{".hidden"});
}

TEST_CASE("virtual path: must be valid UTF-8, which passes through unchanged") {
  CHECK(normalize_virtual_path("日本語/セリフ.txt") ==
        std::optional<std::string>{"日本語/セリフ.txt"});
  CHECK_FALSE(normalize_virtual_path("bad\xff.txt").has_value());
  CHECK_FALSE(normalize_virtual_path("truncated\xe6\x97").has_value());
  CHECK_FALSE(normalize_virtual_path("overlong\xc0\xaf").has_value());
  CHECK_FALSE(normalize_virtual_path("surrogate\xed\xa0\x80").has_value());
}

TEST_CASE("virtual path: case is preserved, never folded") {
  CHECK(normalize_virtual_path("Images/BG.png") == std::optional<std::string>{"Images/BG.png"});
}

TEST_CASE("virtual path: the root is the empty path") {
  CHECK(normalize_virtual_path("") == std::optional<std::string>{""});
  CHECK(normalize_virtual_path("./a/..") == std::optional<std::string>{""});
}
