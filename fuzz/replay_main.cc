// Replays a fuzz target's saved corpus: calls LLVMFuzzerTestOneInput on every file in the
// directories given as arguments. It lets every preset run the corpus, including on compilers
// without libFuzzer, such as Apple clang (D34; TESTING.md 3.3). A target that finds a bug aborts
// through DDS_CHECK or a sanitizer, which fails the test.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
  std::vector<std::filesystem::path> files;
  for (int i = 1; i < argc; ++i) {
    for (const auto& entry : std::filesystem::directory_iterator(argv[i])) {
      if (entry.is_regular_file()) files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto& path : files) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  }
  std::printf("replayed %zu inputs\n", files.size());
  return files.empty() ? 1 : 0;  // an empty corpus means the test is not testing anything
}
