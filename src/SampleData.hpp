#pragma once
#include <string>
#include <vector>
#include <cstddef>

// 一段载入（或录制）的音频，以及切片所需的预分析结果。
// 由 UI 线程创建和分析，之后交给音频线程只读使用。
struct SampleData {
	static const int PEAKS = 1024;

	std::vector<float> left;
	// 单声道时为空
	std::vector<float> right;
	int frames = 0;
	float sampleRate = 44100.f;
	std::string name;
	bool recorded = false;

	// 波形概览（单声道混合后的最小/最大值）
	float peakMin[PEAKS] = {};
	float peakMax[PEAKS] = {};
	// 瞬态检测候选点（帧位置），按强度从高到低排序
	std::vector<int> onsets;

	const float* L() const {
		return left.data();
	}
	const float* R() const {
		return right.empty() ? left.data() : right.data();
	}
	bool stereo() const {
		return !right.empty();
	}

	// 计算波形概览和瞬态候选点（耗时，只在 UI 线程调用）
	void analyze();

	// 计算切片起点；out 需要至少 want + 1 个元素，out[count] = frames。
	// 返回实际切片数（瞬态模式下可能少于 want）。实时安全，可在音频线程调用。
	int computeSlices(int want, bool transient, int* out) const;

private:
	float mono(int i) const {
		return right.empty() ? left[i] : 0.5f * (left[i] + right[i]);
	}
	int snapToZeroCrossing(int pos, int radius) const;
};
