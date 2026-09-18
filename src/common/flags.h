#pragma once

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace md {

// Minimal --key=value command-line parser.
//
// Hand-rolled rather than pulling in a flags library: the whole surface is a
// handful of strings and integers, and each service's flags stay visible in one
// place in its main() instead of being scattered across translation units as
// global registrations.
//
// What it will NOT do is fail quietly. The operator surface is the one a
// reviewer actually types into, and it validates as strictly as the gRPC
// surface does -- for the same reason stated there: a typo must never be
// silently ignored, or the operator gets a plausible system built from
// configuration they did not ask for.
class Flags {
 public:
  Flags(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string argument(argv[i]);
      if (argument.rfind("--", 0) != 0) {
        // A bare word. Almost always a space-separated value -- `--foo 5`
        // instead of `--foo=5` -- which would otherwise be dropped on the floor
        // while `foo` silently became the string "true".
        Fail("unexpected argument '" + argument +
             "'; flags take the form --key=value, not --key value");
      }
      argument.erase(0, 2);
      const std::size_t equals = argument.find('=');
      if (equals == std::string::npos) {
        values_[argument] = "true";
      } else {
        values_[argument.substr(0, equals)] = argument.substr(equals + 1);
      }
    }
  }

  // Rejects any flag the caller does not recognise. Each main() already lists
  // its flags in --help, so this is that list used twice rather than a second
  // source of truth.
  void RequireKnown(std::span<const std::string_view> known) const {
    for (const auto& [key, value] : values_) {
      bool found = false;
      for (const std::string_view candidate : known) {
        if (key == candidate) {
          found = true;
          break;
        }
      }
      if (!found) {
        std::string message = "unknown flag --" + key + "\nvalid flags are:";
        for (const std::string_view candidate : known) {
          message += "\n  --" + std::string(candidate);
        }
        Fail(message);
      }
    }
  }

  std::string Get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
  }

  // Range-checked. A type check catches malformed input; a range check catches
  // input that parses fine and is still wrong, which this program punishes just
  // as hard -- a negative staleness timeout parses perfectly and then excludes
  // every venue from the merge forever. Every integer flag goes through the
  // bounded form for that reason.
  int GetInt(const std::string& key, int fallback, int minimum, int maximum) const {
    const int value = GetInt(key, fallback);
    if (value < minimum || value > maximum) {
      Fail("--" + key + " expects a value between " + std::to_string(minimum) + " and " +
           std::to_string(maximum) + ", got " + std::to_string(value));
    }
    return value;
  }

  int GetInt(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;

    // Rejects both malformed input and the bare `--key` form, which stores
    // "true". Silently yielding 0 here is how `--staleness-ms 5000` used to
    // produce a zero timeout, which excludes every venue from the merge
    // permanently while every venue is plainly healthy -- a visible symptom
    // with an invisible cause.
    int parsed = 0;
    const std::string& text = it->second;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) {
      Fail("--" + key + " expects an integer, got '" + text + "'");
    }
    return parsed;
  }

  bool GetBool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    const std::string& text = it->second;
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
    Fail("--" + key + " expects a boolean, got '" + text + "'");
    return fallback;
  }

  void RequireKnown(std::initializer_list<std::string_view> known) const {
    RequireKnown(std::span<const std::string_view>(known.begin(), known.size()));
  }

  bool Has(const std::string& key) const { return values_.count(key) > 0; }

 private:
  [[noreturn]] static void Fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(2);
  }

  std::map<std::string, std::string> values_;
};

}  // namespace md
