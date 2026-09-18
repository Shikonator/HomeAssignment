#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace md {

// Minimal --key=value command-line parser.
//
// Deliberately hand-rolled rather than pulling in a flags library: the whole
// surface is a handful of strings and integers, and every service's flags are
// then visible in one place in its main() instead of scattered across
// translation units as global registrations.
class Flags {
 public:
  Flags(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string argument(argv[i]);
      if (argument.rfind("--", 0) != 0) continue;
      argument.erase(0, 2);
      const std::size_t equals = argument.find('=');
      if (equals == std::string::npos) {
        values_[argument] = "true";
      } else {
        values_[argument.substr(0, equals)] = argument.substr(equals + 1);
      }
    }
  }

  std::string Get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
  }

  int GetInt(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : std::atoi(it->second.c_str());
  }

  bool GetBool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    return it->second == "true" || it->second == "1" || it->second == "yes";
  }

  bool Has(const std::string& key) const { return values_.count(key) > 0; }

 private:
  std::map<std::string, std::string> values_;
};

}  // namespace md
