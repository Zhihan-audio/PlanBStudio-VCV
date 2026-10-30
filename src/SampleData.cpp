#include "SampleData.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

void SampleData::analyze() {
	frames = (int) left.size();
	if (!right.empty() && right.size() != left.size())
		right.resize(left.size());

	// 波形概览
	for (int b = 0; b < PEAKS; b++) {
		if (frames == 0) {
			peakMin[b] = peakMax[b] = 0.f;
			continue;
		}
		int a = (int) ((int64_t) b * frames / PEAKS);
		int e = (int) ((int64_t) (b + 1) * frames / PEAKS);
		a = std::min(a, frames - 1);
		e = std::max(e, a + 1);
		e = std::min(e, frames);
		float mn = 1e9f, mx = -1e9f;
		for (int i = a; i < e; i++) {
			float v = mono(i);
			mn = std::min(mn, v);
			mx = std::max(mx, v);
		}
		peakMin[b] = mn;
		peakMax[b] = mx;
	}

	// 瞬态检测：带高频加重的短时能量，取对数能量的正向跳变（dB）作为起音强度
	onsets.clear();
	int hop = std::max(32, (int) (sampleRate * 0.004f));
	int win = hop * 2;
	if (frames < win * 3)
		return;
	int nf = (frames - win) / hop + 1;
	std::vector<float> logE(nf);
	float maxDb = -200.f;
	for (int k = 0; k < nf; k++) {
		int start = k * hop;
		float prev = start > 0 ? mono(start - 1) : 0.f;
		double e = 0.0;
		for (int i = start; i < start + win; i++) {
			float x = mono(i);
			float dx = x - prev;
			prev = x;
			e += x * x + 2.f * dx * dx;
		}
		logE[k] = 10.f * std::log10((float) (e / win) + 1e-12f);
		maxDb = std::max(maxDb, logE[k]);
	}

	std::vector<float> flux(nf, 0.f);
	for (int k = 1; k < nf; k++) {
		float f = logE[k] - logE[k - 1];
		if (k >= 2)
			f = std::max(f, 0.6f * (logE[k] - logE[k - 2]));
		flux[k] = std::max(0.f, f);
	}

	const float floorDb = maxDb - 45.f;
	const int gap = std::max(1, (int) (0.035f * sampleRate / hop));
	const int avgR = std::max(2, (int) (0.1f * sampleRate / hop));
	const int minPos = (int) (0.02f * sampleRate);
	const int backoff = (int) (0.001f * sampleRate);
	const int snapR = (int) (0.0007f * sampleRate);

	struct Cand {
		int pos;
		float strength;
	};
	std::vector<Cand> cands;
	for (int k = 1; k < nf; k++) {
		float f = flux[k];
		if (logE[k] < floorDb || f < 2.5f)
			continue;
		// 自适应阈值
		int a = std::max(1, k - avgR), b = std::min(nf - 1, k + avgR);
		float mean = 0.f;
		for (int j = a; j <= b; j++)
			mean += flux[j];
		mean /= (b - a + 1);
		if (f < mean * 1.5f + 1.f)
			continue;
		// 局部最大值（最小间隔 35ms）
		bool isMax = true;
		for (int j = std::max(1, k - gap); j <= std::min(nf - 1, k + gap); j++) {
			if (j != k && (flux[j] > f || (flux[j] == f && j < k))) {
				isMax = false;
				break;
			}
		}
		if (!isMax)
			continue;

		// 在该帧附近找到真正的起音采样点，再往前退 1ms 保留完整的冲击
		int r0 = k * hop;
		int r1 = std::min(frames, r0 + win + hop);
		float peak = 0.f;
		for (int i = r0; i < r1; i++)
			peak = std::max(peak, std::fabs(mono(i)));
		int pos = r0;
		float thr = 0.3f * peak;
		for (int i = r0; i < r1; i++) {
			if (std::fabs(mono(i)) >= thr) {
				pos = i;
				break;
			}
		}
		pos = std::max(0, pos - backoff);
		pos = snapToZeroCrossing(pos, snapR);
		if (pos < minPos || pos >= frames - 1)
			continue;
		float loud = std::min(1.f, std::max(0.1f, (logE[k] - floorDb) / 45.f));
		cands.push_back({pos, f * loud});
	}

	std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
		return a.strength > b.strength;
	});
	if (cands.size() > 512)
		cands.resize(512);
	// 去掉离更强起音点太近（< 20ms）的候选
	const int minDist = (int) (0.02f * sampleRate);
	for (const Cand& c : cands) {
		bool tooClose = false;
		for (int p : onsets) {
			if (std::abs(p - c.pos) < minDist) {
				tooClose = true;
				break;
			}
		}
		if (!tooClose)
			onsets.push_back(c.pos);
		if (onsets.size() >= 256)
			break;
	}
}

int SampleData::snapToZeroCrossing(int pos, int radius) const {
	for (int d = 0; d <= radius; d++) {
		int i = pos - d;
		if (i >= 1 && i < frames && ((mono(i - 1) <= 0.f) != (mono(i) <= 0.f)))
			return i;
		i = pos + d;
		if (i >= 1 && i < frames && ((mono(i - 1) <= 0.f) != (mono(i) <= 0.f)))
			return i;
	}
	return pos;
}

int SampleData::computeSlices(int want, bool transient, int* out) const {
	out[0] = 0;
	if (frames <= 0 || want < 1)
		return 0;
	int count;
	if (transient) {
		int take = std::min(want - 1, (int) onsets.size());
		for (int i = 0; i < take; i++)
			out[i + 1] = onsets[i];
		std::sort(out + 1, out + 1 + take);
		count = take + 1;
	}
	else {
		int radius = (int) (sampleRate * 0.0015f);
		for (int k = 1; k < want; k++) {
			int p = (int) ((int64_t) k * frames / want);
			p = snapToZeroCrossing(p, radius);
			out[k] = std::max(p, out[k - 1]);
		}
		count = want;
	}
	out[count] = frames;
	return count;
}
