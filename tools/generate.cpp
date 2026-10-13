#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sndfile.h>
#include <vector>

int main(int argc, char **argv)
{
	if (argc != 2)
		return 1;
	std::filesystem::create_directories(argv[1]);
	const char *names[] = {"normal", "space", "enter", "backspace", "modifier", "special"};
	uint32_t seed = 183711;
	auto noise = [&]
	{
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		return double(seed) / 4294967296. * 2 - 1;
	};
	for (int c = 0; c < 6; ++c)
		for (int v = 0; v < 5; ++v)
		{
			std::vector<float> s(7200);
			double filtered = 0, softened = 0, body = 0;
			double hz =
				std::array<double, 6>{180, 135, 155, 175, 200, 165}[c] * (1 + .025 * (v - 2));
			for (size_t i = 0; i < s.size(); ++i)
			{
				double t = double(i) / 48000;
				// Cascaded low-pass stages remove the sharp click component.
				// A short cosine attack rounds the contact without leading silence.
				filtered += .080 * (noise() - filtered);
				softened += .050 * (filtered - softened);
				body += .028 * (softened - body);
				double attack = .5 - .5 * std::cos(3.141592653589793 * std::min(t / .0022, 1.));
				double decay = std::exp(-t * std::array<double, 6>{75, 48, 60, 75, 95, 70}[c]);
				double thock =
					std::sin(6.283185307 * hz * (t - .35 * t * t)) * std::exp(-t * 150) * .045;
				double recoil = t > .009 ? body * std::exp(-(t - .009) * 120) * .22 : 0;
				s[i] = float(attack * ((softened * .65 + body * .95) * decay + thock + recoil));
				s[i] *= float(std::min(1., (s.size() - i) / 480.));
			}
			SF_INFO info{};
			info.samplerate = 48000;
			info.channels = 1;
			info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
			auto path = std::filesystem::path(argv[1]) / (std::string(names[c]) + "-" + std::to_string(v + 1) + ".wav");
			auto f = sf_open(path.c_str(), SFM_WRITE, &info);
			if (!f)
				return 2;
			sf_writef_float(f, s.data(), s.size());
			sf_close(f);
		}
	std::cout << "Generated 30 original smooth linear-style synthesized variations\n";
}
