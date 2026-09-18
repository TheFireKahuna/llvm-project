// File, encoding and locale boundaries between libc++ and the UCRT.
// RUN: %clangxx_crt_main -std=c++20 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe %t.data
// REQUIRES: windows, crt

#include <assert.h>
#include <clocale>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>

int main(int argc, char **argv) {
  assert(argc == 2);
  namespace fs = std::filesystem;
  const fs::path path = fs::path(argv[1]).concat(L"_\u65e5\u672c.txt");
  const std::string bytes("a\0b\n\r\n\xC3\xA9", 8);
  FILE *file = _wfopen(path.c_str(), L"wb");
  assert(file && fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  assert(fclose(file) == 0);
  {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::string read((std::istreambuf_iterator<char>(input)), {});
    assert(read == bytes);
  }
  {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << "tail";
    output.close();
    assert(output);
  }
  assert(fs::file_size(path) == bytes.size() + 4);
  const fs::path renamed = fs::path(path).concat(L".renamed");
  fs::rename(path, renamed);
  assert(!fs::exists(path) && fs::exists(renamed));
  assert(fs::remove(renamed));

  assert(std::setlocale(LC_ALL, ".UTF8"));
  const std::locale utf8(".UTF8");
  assert(std::has_facet<std::ctype<wchar_t>>(utf8));
  std::wostringstream output;
  output.imbue(utf8);
  output << 1234 << L' ' << 1.5;
  std::wistringstream input(output.str());
  input.imbue(utf8);
  int integer = 0;
  double real = 0;
  input >> integer >> real;
  assert(input && integer == 1234 && real == 1.5);
  assert(std::setlocale(LC_ALL, "C"));
}
