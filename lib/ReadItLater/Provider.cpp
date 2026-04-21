#include "Provider.h"

const char* Provider::errorString(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::NO_TOKENS:
      return "No credentials";
    case Result::AUTH_FAILED:
      return "Auth failed";
    case Result::NETWORK_FAILED:
      return "Network error";
    case Result::PARSE_FAILED:
      return "Response parse error";
    case Result::RATE_LIMITED:
      return "Rate limited";
    case Result::SERVER_ERROR:
      return "Server error";
  }
  return "Unknown";
}
