#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "engine/core/no_crt_dialogs.hpp"

int main(int argc, char** argv) {
  vnmaki::disable_crt_dialogs();
  return doctest::Context(argc, argv).run();
}
