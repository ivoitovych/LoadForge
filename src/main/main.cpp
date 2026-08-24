// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <iostream>
#include <span>

#include "main/cli.hpp"

int main(int argc, char** argv) {
  const std::span<char* const> raw{argv, static_cast<std::size_t>(argc)};
  return loadforge::cli::run(loadforge::cli::collect_args(raw), std::cout, std::cerr);
}
