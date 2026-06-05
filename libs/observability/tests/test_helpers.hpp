#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace test_helpers {

inline std::string write_fake_helper(const std::filesystem::path& dir, const std::string& json_payload) {
  const auto script = dir / "fake_adlx.cmd";
  std::ofstream out(script);
  out << "@echo off\r\n";
  out << "echo " << json_payload << "\r\n";
  out.close();
  return script.string();
}

inline std::filesystem::path make_temp_dir(const std::string& tag) {
  static int counter = 0;
  const auto base = std::filesystem::temp_directory_path() /
                    ("inferdeck_test_" + tag + "_" + std::to_string(++counter));
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base);
  return base;
}

}
