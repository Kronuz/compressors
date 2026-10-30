// Replays the corpus files and generated inputs through a fuzz target without libFuzzer, so the
// same checks run as a test.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
	std::size_t runs = 0;
	if (argc > 1) {
		for (const auto& e : std::filesystem::recursive_directory_iterator(argv[1])) {
			if (!e.is_regular_file()) { continue; }
			std::ifstream f(e.path(), std::ios::binary);
			std::vector<std::uint8_t> in((std::istreambuf_iterator<char>(f)), {});
			LLVMFuzzerTestOneInput(in.data(), in.size());
			++runs;
		}
	}
	std::mt19937_64 rng(20261030);
	std::vector<std::uint8_t> input;
	for (std::size_t size : {0, 1, 3, 4, 5, 16, 64, 300, 2047, 4100, 9000}) {
		for (int round = 0; round < 40; ++round) {
			input.resize(size);
			for (auto& b : input) { b = static_cast<std::uint8_t>(rng()); }
			LLVMFuzzerTestOneInput(input.data(), input.size());
			++runs;
		}
	}
	std::printf("fuzz replay: %zu inputs, ok\n", runs);
}
