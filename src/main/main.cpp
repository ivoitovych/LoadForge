// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <string_view>
#include <vector>

#include "main/cli.hpp"

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return loadforge::cli::run(args, std::cout, std::cerr);
}
