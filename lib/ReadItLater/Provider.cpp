#include "Provider.h"

const char* Provider::errorString(Result::Code code) {
  switch (code) {
    case Result::Code::OK:
      return "OK";
    case Result::Code::NO_TOKENS:
      return "No credentials";
    case Result::Code::AUTH_FAILED:
      return "Auth failed";
    case Result::Code::FORBIDDEN:
      return "Access denied";
    case Result::Code::NOT_FOUND:
      return "Not found";
    case Result::Code::NETWORK_FAILED:
      return "Network error";
    case Result::Code::PARSE_FAILED:
      return "Response parse error";
    case Result::Code::RATE_LIMITED:
      return "Rate limited";
    case Result::Code::INSUFFICIENT_MEMORY:
      return "Device memory low";
    case Result::Code::INTERNAL_ERROR:
      return "Internal error";
    case Result::Code::SERVER_ERROR:
      return "Server error";
  }
  return "Unknown";
}
