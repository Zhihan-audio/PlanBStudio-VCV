#pragma once
#include <string>
#include <vector>
#include <cstddef>

// 依赖自由的音频文件读写：WAV (PCM 8/16/24/32, float 32/64, Extensible) 与 AIFF / AIFF-C
namespace audiofile {

struct Decoded {
	std::vector<float> left;
	// 单声道文件时为空
	std::vector<float> right;
	float sampleRate = 44100.f;
	std::string error;
};

bool load(const std::string& path, Decoded& out);

// 写 32-bit float WAV；right 为 NULL 时写单声道
bool writeWav(const std::string& path, const float* left, const float* right, size_t frames, float sampleRate);

} // namespace audiofile
