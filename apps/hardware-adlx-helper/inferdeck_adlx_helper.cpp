#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

static std::string now_iso() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

static void unavailable(const std::string& reason) {
  std::cout
    << "{"
    << "\"available\":false,"
    << "\"provider\":\"amd_adlx\","
    << "\"reason\":\"" << reason << "\","
    << "\"timestamp\":\"" << now_iso() << "\""
    << "}\n";
}

int main(int argc, char** argv) {
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--json") json = true;
  }
  if (!json) {
    std::cerr << "Usage: inferdeck-adlx-helper --json\n";
    return 2;
  }

#ifndef _WIN32
  unavailable("adlx_windows_only");
  return 0;
#endif

#ifndef INFERDECK_WITH_ADLX
  unavailable("built_without_adlx_sdk");
  return 0;
#else
  // ADLX SDK integration point. The production build should compile this target
  // on the Windows host with ADLX_SDK_DIR set. Keep the JSON contract stable:
  // available/provider/gpu/cpu/memory/disk/timestamp.
  unavailable("adlx_sdk_integration_not_linked");
  return 0;
#endif
}
