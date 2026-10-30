#include "plugin.hpp"
#include "AudioFile.hpp"
#include "SampleData.hpp"
#include <osdialog.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>

// turn everything into a 碎了吧唧的鼓（内部名 Splinter）—— 32 步切片采样器
//
// 线程模型：
// - UI 线程负责读文件、分析（波形概览/瞬态检测）、写录音文件，然后通过 pendingSample 把 SampleData 交给音频线程。
// - 音频线程在 process() 里换上新的 SampleData，把旧的放进 retiredSample，由 UI 线程在 uiStep() 里删除。
// 这样音频线程里不做任何文件 IO 或大块内存释放。

namespace {

const int NUM_STEPS = 32;
const int MAX_SLICES = 64;
const int NUM_VOICES = 4;
const float MAX_REC_SECONDS = 30.f;
const size_t REC_CAPACITY = (size_t) (MAX_REC_SECONDS * 96000);
// 每拍的步数：1/8、1/16、1/16 三连音、1/32
const int STEP_DIVS[] = {2, 4, 6, 8};
const int REC_BARS[] = {0, 1, 2, 4};
const char* RECORDING_FILE = "recording.wav";

enum RecState {
	REC_IDLE,
	REC_ARMED,
	REC_RUNNING,
	// 录音结束，等待 UI 线程取走并分析
	REC_DONE,
};

// 面板布局（mm），必须和 tools/make_panel.py 保持一致
const float LEFT_X[4] = {10.f, 22.f, 34.f, 46.f};
const float LEFT_Y[7] = {34.f, 48.f, 62.f, 76.f, 90.f, 104.f, 118.f};
// 每步记的是切片编号（从 0 数）。编号超过实际切片数时绕回来：16 片时第 20 号 = 第 4 片。
inline int wrapSlice(int idx, int n) {
	idx %= n;
	return idx < 0 ? idx + n : idx;
}

inline float stepX(int i) {
	return 72.5f + i * 7.5f + (i / 4) * 1.6f;
}
const float STEP_COL_W = 7.5f;
const float STEP_AREA_TOP = 40.f;
const float STEP_AREA_BOTTOM = 117.5f;
const float Y_NUM = 43.5f;
const float Y_ON = 49.5f;
const float Y_SLICE = 60.f;
const float Y_SLICE_TXT = 65.9f;
const float Y_PITCH = 73.5f;
const float Y_PITCH_TXT = 79.4f;
const float Y_VOL = 87.f;
const float Y_RATCHET = 99.5f;
const float Y_RATCHET_TXT = 105.4f;
const float Y_MODE = 112.5f;

// 4 点 Hermite 插值
inline float hermite(const float* buf, int frames, double pos) {
	int i = (int) pos;
	float t = (float) (pos - i);
	int last = frames - 1;
	float xm1 = buf[clamp(i - 1, 0, last)];
	float x0 = buf[clamp(i, 0, last)];
	float x1 = buf[clamp(i + 1, 0, last)];
	float x2 = buf[clamp(i + 2, 0, last)];
	float c1 = 0.5f * (x1 - xm1);
	float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
	float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
	return ((c3 * t + c2) * t + c1) * t + x0;
}

} // namespace

struct SliceQuantity : ParamQuantity {
	std::string getDisplayValueString() override;
	void setDisplayValueString(std::string s) override;
};

struct RateQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		if (v >= 0.999f)
			return "Off (full rate)";
		float sr = APP->engine->getSampleRate();
		return string::f("%.0f Hz", sr * std::exp2(-(1.f - v) * 7.f));
	}
	void setDisplayValueString(std::string s) override {
		float hz = std::atof(s.c_str());
		if (hz <= 0.f)
			return;
		float sr = APP->engine->getSampleRate();
		setValue(clamp(1.f + std::log2(hz / sr) / 7.f, 0.f, 1.f));
	}
};

struct Voice {
	enum Stage {
		ATTACK,
		DECAY,
		RELEASE,
	};
	bool active = false;
	bool perc = false;
	bool choking = false;
	double pos = 0.0;
	double end = 0.0;
	float rate = 1.f;
	float gain = 1.f;
	float choke = 1.f;
	float env = 0.f;
	float penv = 0.f;
	float gateLeft = 0.f;
	int stage = ATTACK;
	int age = 0;
	int slice = -1;
};

struct Splinter : Module {
	enum ParamId {
		BPM_PARAM,
		SWING_PARAM,
		LENGTH_PARAM,
		LEVEL_PARAM,
		SLICES_PARAM,
		SLICEMODE_PARAM,
		BITS_PARAM,
		RATE_PARAM,
		ATTACK_PARAM,
		DECAY_PARAM,
		SUSTAIN_PARAM,
		RELEASE_PARAM,
		PUNCH_PARAM,
		DRIVE_PARAM,
		RUN_PARAM,
		RESET_PARAM,
		REC_PARAM,
		ENUMS(ON_PARAM, NUM_STEPS),
		ENUMS(SLICE_PARAM, NUM_STEPS),
		ENUMS(PITCH_PARAM, NUM_STEPS),
		ENUMS(VOL_PARAM, NUM_STEPS),
		ENUMS(RATCHET_PARAM, NUM_STEPS),
		ENUMS(MODE_PARAM, NUM_STEPS),
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		RUN_INPUT,
		REC_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		SLICE_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		TRIG_OUTPUT,
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	// 步进开关和按钮是自绘的 pad，直接读参数；只有录音灯需要闪烁状态
	enum LightId {
		REC_LIGHT,
		LIGHTS_LEN
	};

	// ---- 采样数据交接 ----
	std::atomic<SampleData*> current{nullptr};
	std::atomic<SampleData*> pendingSample{nullptr};
	std::atomic<SampleData*> retiredSample{nullptr};
	std::atomic<bool> clearRequest{false};
	std::atomic<uint32_t> sampleSerial{0};

	// ---- 只在 UI 线程使用 ----
	SampleData* uiSample = nullptr;
	std::string samplePath;
	std::string sampleName;
	bool sampleRecorded = false;
	std::string loadError;
	std::string lastDir;
	bool deferredRecordingLoad = false;

	// ---- 切片（音频线程写，显示读）----
	int sliceStart[MAX_SLICES + 1] = {};
	int numSlices = 0;
	bool slicesDirty = true;
	int lastSlicesWant = -1;
	int lastSliceMode = -1;

	// ---- 音序器 ----
	int currentStep = -1;
	double phase = 1.0;
	float stepDur = 0.125f;
	float extStepDur = 0.125f;
	float clockTimer = 0.f;
	float stepTime = 0.f;
	float hitDelay = 0.f;
	float hitWindow = 0.125f;
	int hitsTotal = 0;
	int hitsDone = 0;
	bool prevRunning = false;
	bool externalClock = false;
	int stepDivision = 1;
	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger runTrigger;
	dsp::SchmittTrigger recTrigger;
	dsp::BooleanTrigger resetButton;
	dsp::BooleanTrigger recButton;
	dsp::PulseGenerator trigPulse;

	// ---- 发声 ----
	Voice voices[NUM_VOICES];
	float fadeInLen = 16.f;
	float fadeInInv = 1.f / 16.f;
	float fadeOutLen = 88.f;
	float fadeOutInv = 1.f / 88.f;
	float chokeStep = 1.f / 132.f;
	float attackInc = 1.f;
	float decayCoef = 0.999f;
	float sustainLevel = 0.f;
	float releaseCoef = 0.999f;
	float punchSemis = 0.f;
	float punchCoef = 0.99f;
	float drive = 0.f;
	float driveGain = 1.f;
	float driveNorm = 1.f;
	float crushLevels = 32768.f;
	bool crushOn = false;
	float holdRatio = 1.f;
	float holdPhase = 0.f;
	float heldL = 0.f;
	float heldR = 0.f;
	float level = 1.f;

	// ---- 录音 ----
	std::unique_ptr<float[]> recL;
	std::unique_ptr<float[]> recR;
	size_t recPos = 0;
	size_t recTarget = 0;
	bool recStereo = false;
	float recSampleRate = 44100.f;
	std::atomic<int> recState{REC_IDLE};
	int recLengthMode = 0;

	// ---- 给界面显示的读数 ----
	float uiBpm = 120.f;
	float uiPlayPos = -1.f;
	int uiPlaySlice = -1;
	float blinkPhase = 0.f;

	dsp::ClockDivider paramDivider;
	dsp::ClockDivider lightDivider;

	Splinter() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(BPM_PARAM, 20.f, 300.f, 120.f, "Tempo 速度", " BPM");
		configParam(SWING_PARAM, 0.5f, 0.75f, 0.5f, "Swing 摇摆", "%", 0.f, 100.f);
		configParam(LENGTH_PARAM, 1.f, (float) NUM_STEPS, (float) NUM_STEPS, "Steps 步数");
		getParamQuantity(LENGTH_PARAM)->snapEnabled = true;
		configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Output level 输出电平", " dB", -10.f, 20.f);

		configParam(SLICES_PARAM, 1.f, (float) MAX_SLICES, (float) NUM_STEPS, "Slices 切片数 (切片精度)");
		getParamQuantity(SLICES_PARAM)->snapEnabled = true;
		configSwitch(SLICEMODE_PARAM, 0.f, 1.f, 0.f, "Slice mode 切片方式", {"Grid 等分", "Transient 瞬态检测"});
		configParam(BITS_PARAM, 1.f, 16.f, 16.f, "Bit depth 位深 (采样精度)", " bit");
		configParam<RateQuantity>(RATE_PARAM, 0.f, 1.f, 1.f, "Sample rate 降采样");

		configParam(ATTACK_PARAM, 0.f, 1.f, 0.25f, "Perc attack 起音", " ms", 500.f, 0.2f);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Perc decay 衰减", " ms", 400.f, 5.f);
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.f, "Perc sustain 持续", "%", 0.f, 100.f);
		configParam(RELEASE_PARAM, 0.f, 1.f, 0.3f, "Perc release 释放", " ms", 400.f, 5.f);
		configParam(PUNCH_PARAM, 0.f, 1.f, 0.3f, "Perc punch 冲击 (音高包络)", "%", 0.f, 100.f);
		configParam(DRIVE_PARAM, 0.f, 1.f, 0.2f, "Perc drive 饱和", "%", 0.f, 100.f);

		configSwitch(RUN_PARAM, 0.f, 1.f, 1.f, "Run 运行", {"Stopped 停止", "Running 运行中"});
		configButton(RESET_PARAM, "Reset 复位");
		configButton(REC_PARAM, "Record 录音 (从 IN 采样)");

		for (int i = 0; i < NUM_STEPS; i++) {
			std::string n = string::f("Step %d ", i + 1);
			configSwitch(ON_PARAM + i, 0.f, 1.f, 1.f, n + "on 开关", {"Off 关", "On 开"});
			// 默认第 N 步 = 第 N 片
			configParam<SliceQuantity>(SLICE_PARAM + i, 1.f, (float) MAX_SLICES, (float) (i + 1), n + "slice 切片");
			getParamQuantity(SLICE_PARAM + i)->snapEnabled = true;
			configParam(PITCH_PARAM + i, -24.f, 24.f, 0.f, n + "pitch 音高", " st");
			getParamQuantity(PITCH_PARAM + i)->snapEnabled = true;
			configParam(VOL_PARAM + i, 0.f, 1.f, 1.f, n + "level 音量", " dB", -10.f, 40.f);
			configParam(RATCHET_PARAM + i, 1.f, 8.f, 1.f, n + "ratchet 连击", "x");
			getParamQuantity(RATCHET_PARAM + i)->snapEnabled = true;
			configSwitch(MODE_PARAM + i, 0.f, 1.f, 0.f, n + "mode 模式", {"Normal 正常播放", "Perc 打击乐 ADSR"});
		}

		configInput(CLOCK_INPUT, "Clock 时钟 (每个脉冲 = 1 步)");
		configInput(RESET_INPUT, "Reset 复位");
		configInput(RUN_INPUT, "Run toggle 运行切换");
		configInput(REC_INPUT, "Record toggle 录音切换");
		configInput(IN_L_INPUT, "Audio in L 录音输入 左");
		configInput(IN_R_INPUT, "Audio in R 录音输入 右");
		configInput(SLICE_CV_INPUT, "Slice offset CV 切片偏移 (0-10V = 整段)");
		configOutput(TRIG_OUTPUT, "Trigger 每次触发");
		configOutput(OUT_L_OUTPUT, "Audio L");
		configOutput(OUT_R_OUTPUT, "Audio R");

		recL.reset(new float[REC_CAPACITY]);
		recR.reset(new float[REC_CAPACITY]);

		paramDivider.setDivision(32);
		lightDivider.setDivision(128);
		setupRates(44100.f);
	}

	~Splinter() {
		delete pendingSample.exchange(nullptr);
		delete retiredSample.exchange(nullptr);
		delete current.exchange(nullptr);
	}

	void setupRates(float sr) {
		fadeInLen = std::max(1.f, 0.0004f * sr);
		fadeInInv = 1.f / fadeInLen;
		fadeOutLen = std::max(1.f, 0.002f * sr);
		fadeOutInv = 1.f / fadeOutLen;
		chokeStep = 1.f / std::max(1.f, 0.003f * sr);
		updateEnvParams(sr);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		for (Voice& v : voices)
			v.active = false;
		setupRates(e.sampleRate);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		resetSequence();
	}

	// Ctrl+R：偏 IDM 的随机化，只动每一步的参数，不动速度/包络等全局设置
	void onRandomize(const RandomizeEvent& e) override {
		static const int intervals[] = {-12, -7, -5, -2, 2, 3, 5, 7, 12};
		for (int i = 0; i < NUM_STEPS; i++) {
			params[ON_PARAM + i].setValue(random::uniform() < 0.8f ? 1.f : 0.f);
			params[SLICE_PARAM + i].setValue(1 + random::u32() % sliceCount());
			float r = random::uniform();
			int ratchet = r < 0.7f ? 1 : r < 0.84f ? 2 : r < 0.9f ? 3 : r < 0.95f ? 4 : r < 0.98f ? 6 : 8;
			params[RATCHET_PARAM + i].setValue(ratchet);
			params[MODE_PARAM + i].setValue(random::uniform() < 0.35f ? 1.f : 0.f);
			params[PITCH_PARAM + i].setValue(random::uniform() < 0.7f ? 0.f : intervals[random::u32() % 9]);
			params[VOL_PARAM + i].setValue(0.6f + 0.4f * random::uniform());
		}
	}

	// 当前实际切片数；还没载入采样时用 SLICES 旋钮的值
	int sliceCount() {
		int n = numSlices > 0 ? numSlices : (int) params[SLICES_PARAM].getValue();
		return std::max(1, n);
	}

	void randomizeSlices() {
		int n = sliceCount();
		for (int i = 0; i < NUM_STEPS; i++)
			params[SLICE_PARAM + i].setValue(1 + random::u32() % n);
	}

	void resetSliceOrder() {
		for (int i = 0; i < NUM_STEPS; i++)
			params[SLICE_PARAM + i].setValue(i + 1);
	}

	void resetSequence() {
		currentStep = -1;
		phase = 1.0;
		hitsTotal = 0;
		hitsDone = 0;
	}

	void updateEnvParams(float sr) {
		float a = 0.2e-3f * std::pow(500.f, params[ATTACK_PARAM].getValue());
		attackInc = 1.f / std::max(1.f, a * sr);
		// 衰减/释放时间定义为降到 1%（-40dB）所需时间
		float d = 5e-3f * std::pow(400.f, params[DECAY_PARAM].getValue());
		decayCoef = std::exp(-4.6f / (d * sr));
		sustainLevel = params[SUSTAIN_PARAM].getValue();
		float r = 5e-3f * std::pow(400.f, params[RELEASE_PARAM].getValue());
		releaseCoef = std::exp(-4.6f / (r * sr));

		float punch = params[PUNCH_PARAM].getValue();
		punchSemis = punch * 24.f;
		punchCoef = std::exp(-1.f / ((0.006f + 0.025f * punch) * sr));

		drive = params[DRIVE_PARAM].getValue();
		driveGain = 1.f + drive * drive * 24.f;
		driveNorm = 1.f / std::tanh(driveGain);

		float bits = params[BITS_PARAM].getValue();
		crushOn = bits < 15.99f;
		crushLevels = std::pow(2.f, bits - 1.f);
		float rate = params[RATE_PARAM].getValue();
		holdRatio = rate >= 0.999f ? 1.f : std::exp2(-(1.f - rate) * 7.f);

		level = params[LEVEL_PARAM].getValue();
	}

	// ---------------- 音频线程 ----------------

	void handleSampleSwap() {
		SampleData* p = pendingSample.exchange(nullptr);
		bool clear = clearRequest.exchange(false);
		if (!p && !clear)
			return;
		SampleData* old = current.exchange(p);
		for (Voice& v : voices)
			v.active = false;
		slicesDirty = true;
		sampleSerial++;
		if (old) {
			// 正常情况下交给 UI 线程删除；极少数情况下 UI 来不及回收，就在这里删
			delete retiredSample.exchange(old);
		}
	}

	void updateSlices() {
		int want = (int) params[SLICES_PARAM].getValue();
		int mode = params[SLICEMODE_PARAM].getValue() > 0.5f ? 1 : 0;
		if (!slicesDirty && want == lastSlicesWant && mode == lastSliceMode)
			return;
		slicesDirty = false;
		lastSlicesWant = want;
		lastSliceMode = mode;
		SampleData* s = current.load(std::memory_order_relaxed);
		numSlices = s ? s->computeSlices(clamp(want, 1, MAX_SLICES), mode, sliceStart) : 0;
	}

	void advanceStep(float sr) {
		int len = (int) params[LENGTH_PARAM].getValue();
		currentStep++;
		if (currentStep < 0 || currentStep >= len)
			currentStep = 0;
		stepTime = 0.f;

		// 摇摆：把偶数位（第 2、4、6… 个 16 分音符）往后推
		float swing = clamp((params[SWING_PARAM].getValue() - 0.5f) * 2.f, 0.f, 0.5f) * stepDur;
		bool offbeat = currentStep & 1;
		hitDelay = offbeat ? swing : 0.f;
		hitWindow = offbeat ? stepDur - swing : stepDur + swing;
		hitsDone = 0;
		bool on = params[ON_PARAM + currentStep].getValue() > 0.5f;
		hitsTotal = on ? (int) params[RATCHET_PARAM + currentStep].getValue() : 0;

		if (recState.load(std::memory_order_relaxed) == REC_ARMED && currentStep == 0)
			startRecording(sr);
	}

	void fireHit(SampleData* s, float sr, int step, float gateTime) {
		trigPulse.trigger(1e-3f);
		if (!s || numSlices <= 0)
			return;

		int n = numSlices;
		int idx = (int) params[SLICE_PARAM + step].getValue() - 1;
		// SLC CV：0-10V 把所有步整体往后推 0 到一整圈
		if (inputs[SLICE_CV_INPUT].isConnected())
			idx += (int) std::floor(inputs[SLICE_CV_INPUT].getVoltage() / 10.f * n);
		idx = wrapSlice(idx, n);
		float semis = params[PITCH_PARAM + step].getValue();
		float vol = params[VOL_PARAM + step].getValue();
		bool perc = params[MODE_PARAM + step].getValue() > 0.5f;

		// 单音切片：新触发会让之前的声音在 3ms 内淡出（choke），这是碎拍切片的关键质感
		for (Voice& v : voices) {
			if (v.active)
				v.choking = true;
		}
		Voice* nv = nullptr;
		for (Voice& v : voices) {
			if (!v.active) {
				nv = &v;
				break;
			}
		}
		if (!nv) {
			nv = &voices[0];
			for (Voice& v : voices) {
				if (v.choke < nv->choke)
					nv = &v;
			}
		}

		Voice& v = *nv;
		v.active = true;
		v.perc = perc;
		v.choking = false;
		v.choke = 1.f;
		v.pos = sliceStart[idx];
		v.end = sliceStart[idx + 1];
		v.rate = (s->sampleRate / sr) * std::exp2(semis / 12.f);
		v.gain = vol * vol;
		v.env = 0.f;
		v.penv = 1.f;
		v.stage = Voice::ATTACK;
		v.gateLeft = gateTime * sr;
		v.age = 0;
		v.slice = idx;
	}

	inline void renderVoice(Voice& v, SampleData* s, float& outL, float& outR) {
		float r = v.rate;
		if (v.perc && punchSemis > 0.f) {
			r *= dsp::exp2_taylor5(punchSemis * v.penv / 12.f);
			v.penv *= punchCoef;
		}

		float a = hermite(s->L(), s->frames, v.pos);
		float b = s->stereo() ? hermite(s->R(), s->frames, v.pos) : a;

		float g = v.gain;
		// 防爆音：起点 0.4ms 淡入，切片末尾 2ms 淡出
		if (v.age < fadeInLen)
			g *= (v.age + 1) * fadeInInv;
		float remain = (float) ((v.end - v.pos) / r);
		if (remain < fadeOutLen)
			g *= std::max(0.f, remain) * fadeOutInv;
		if (v.choking) {
			v.choke -= chokeStep;
			if (v.choke <= 0.f) {
				v.active = false;
				return;
			}
			g *= v.choke;
		}

		if (v.perc) {
			// 打击乐通道：饱和 → 快速 ADSR → 音高冲击（在上面）
			if (drive > 0.001f) {
				a = a + drive * (std::tanh(a * driveGain) * driveNorm - a);
				b = b + drive * (std::tanh(b * driveGain) * driveNorm - b);
			}
			switch (v.stage) {
				case Voice::ATTACK:
					v.env += attackInc;
					if (v.env >= 1.f) {
						v.env = 1.f;
						v.stage = Voice::DECAY;
					}
					break;
				case Voice::DECAY:
					v.env = sustainLevel + (v.env - sustainLevel) * decayCoef;
					break;
				default:
					v.env *= releaseCoef;
					break;
			}
			v.gateLeft -= 1.f;
			if (v.gateLeft <= 0.f && v.stage != Voice::RELEASE)
				v.stage = Voice::RELEASE;
			bool falling = v.stage == Voice::RELEASE || (v.stage == Voice::DECAY && sustainLevel < 1e-4f);
			if (falling && v.env < 1e-4f) {
				v.active = false;
				return;
			}
			g *= v.env;
		}

		outL += a * g;
		outR += b * g;
		v.pos += r;
		v.age++;
		if (v.pos >= v.end)
			v.active = false;
	}

	void startRecording(float sr) {
		recPos = 0;
		recSampleRate = sr;
		size_t maxFrames = std::min(REC_CAPACITY, (size_t) (MAX_REC_SECONDS * sr));
		recTarget = maxFrames;
		int bars = REC_BARS[clamp(recLengthMode, 0, 3)];
		if (bars > 0) {
			int div = STEP_DIVS[clamp(stepDivision, 0, 3)];
			double frames = (double) bars * 4 * div * stepDur * sr;
			recTarget = std::min(maxFrames, (size_t) std::max(1.0, frames));
		}
		recStereo = inputs[IN_L_INPUT].isConnected() && inputs[IN_R_INPUT].isConnected();
		recState.store(REC_RUNNING);
	}

	void finishRecording() {
		recState.store(recPos > (size_t) (0.02f * recSampleRate) ? REC_DONE : REC_IDLE);
	}

	void toggleRecording(bool running, float sr) {
		int st = recState.load();
		if (st == REC_IDLE) {
			if (!inputs[IN_L_INPUT].isConnected() && !inputs[IN_R_INPUT].isConnected())
				return;
			// 按小节录音且音序器在跑：等到第 1 步再开始，录下来的循环和网格对齐
			if (recLengthMode > 0 && running)
				recState.store(REC_ARMED);
			else
				startRecording(sr);
		}
		else if (st == REC_ARMED) {
			recState.store(REC_IDLE);
		}
		else if (st == REC_RUNNING) {
			finishRecording();
		}
	}

	void process(const ProcessArgs& args) override {
		const float sr = args.sampleRate;
		const float dt = args.sampleTime;

		// 新采样到了就立刻换上（不等分频），保证载入后的第 1 步也能发声
		bool swapNow = pendingSample.load(std::memory_order_relaxed) || clearRequest.load(std::memory_order_relaxed);
		if (paramDivider.process() || swapNow) {
			handleSampleSwap();
			updateSlices();
			updateEnvParams(sr);
		}
		SampleData* s = current.load(std::memory_order_relaxed);

		// 运行 / 复位 / 录音
		bool running = params[RUN_PARAM].getValue() > 0.5f;
		if (runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 2.f)) {
			running = !running;
			params[RUN_PARAM].setValue(running ? 1.f : 0.f);
		}
		if (running && !prevRunning)
			resetSequence();
		prevRunning = running;

		bool resetIn = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
		bool resetBtn = resetButton.process(params[RESET_PARAM].getValue() > 0.5f);
		if (resetIn || resetBtn)
			resetSequence();

		bool recIn = recTrigger.process(inputs[REC_INPUT].getVoltage(), 0.1f, 2.f);
		bool recBtn = recButton.process(params[REC_PARAM].getValue() > 0.5f);
		if (recIn || recBtn)
			toggleRecording(running, sr);

		// 时钟：外部时钟每个脉冲走一步，否则按 BPM
		int div = STEP_DIVS[clamp(stepDivision, 0, 3)];
		bool advance = false;
		if (inputs[CLOCK_INPUT].isConnected()) {
			externalClock = true;
			clockTimer += dt;
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f)) {
				if (clockTimer > 0.002f && clockTimer < 4.f)
					extStepDur = clockTimer;
				clockTimer = 0.f;
				if (running)
					advance = true;
			}
			stepDur = extStepDur;
		}
		else {
			externalClock = false;
			stepDur = 60.f / (params[BPM_PARAM].getValue() * div);
			if (running) {
				phase += dt / stepDur;
				if (phase >= 1.0) {
					phase -= 1.0;
					if (phase >= 1.0)
						phase = 0.0;
					advance = true;
				}
			}
		}
		uiBpm = 60.f / (stepDur * div);

		if (advance)
			advanceStep(sr);

		// 连击：在这一步的时间窗内均匀触发 hitsTotal 次
		if (currentStep >= 0 && hitsDone < hitsTotal) {
			float t = hitDelay + hitsDone * hitWindow / hitsTotal;
			if (stepTime >= t) {
				fireHit(s, sr, currentStep, hitWindow / hitsTotal);
				hitsDone++;
			}
		}
		stepTime += dt;

		// 录音
		if (recState.load(std::memory_order_relaxed) == REC_RUNNING) {
			bool lc = inputs[IN_L_INPUT].isConnected();
			bool rc = inputs[IN_R_INPUT].isConnected();
			float l = lc ? inputs[IN_L_INPUT].getVoltageSum() : inputs[IN_R_INPUT].getVoltageSum();
			float r = rc ? inputs[IN_R_INPUT].getVoltageSum() : l;
			recL[recPos] = l / 5.f;
			recR[recPos] = r / 5.f;
			recPos++;
			if (recPos >= recTarget)
				finishRecording();
		}

		// 发声
		float outL = 0.f, outR = 0.f;
		if (s && s->frames > 0) {
			for (Voice& v : voices) {
				if (v.active)
					renderVoice(v, s, outL, outR);
			}
		}

		// 降采样 + 降位深（“采样精度”）
		if (holdRatio < 1.f) {
			holdPhase += holdRatio;
			if (holdPhase >= 1.f) {
				holdPhase -= 1.f;
				heldL = outL;
				heldR = outR;
			}
			outL = heldL;
			outR = heldR;
		}
		if (crushOn) {
			outL = std::round(outL * crushLevels) / crushLevels;
			outR = std::round(outR * crushLevels) / crushLevels;
		}

		outputs[OUT_L_OUTPUT].setVoltage(clamp(outL * level * 5.f, -12.f, 12.f));
		outputs[OUT_R_OUTPUT].setVoltage(clamp(outR * level * 5.f, -12.f, 12.f));
		outputs[TRIG_OUTPUT].setVoltage(trigPulse.process(dt) ? 10.f : 0.f);

		if (lightDivider.process())
			updateLights(dt * lightDivider.getDivision(), running, s);
	}

	void updateLights(float ldt, bool running, SampleData* s) {
		blinkPhase += ldt * 4.f;
		if (blinkPhase >= 1.f)
			blinkPhase -= 1.f;
		int rs = recState.load(std::memory_order_relaxed);
		float recLight = rs == REC_RUNNING ? 1.f : rs == REC_ARMED ? (blinkPhase < 0.5f ? 1.f : 0.f) : 0.f;
		lights[REC_LIGHT].setBrightness(recLight);

		// 当前发声的切片和播放头位置
		uiPlayPos = -1.f;
		uiPlaySlice = -1;
		if (s && s->frames > 0) {
			for (const Voice& v : voices) {
				if (v.active && !v.choking) {
					uiPlayPos = (float) (v.pos / s->frames);
					uiPlaySlice = v.slice;
				}
			}
		}
	}

	// ---------------- UI 线程 ----------------

	void queueSample(SampleData* s) {
		delete pendingSample.exchange(s);
		uiSample = s;
	}

	bool loadSampleFile(const std::string& path) {
		audiofile::Decoded d;
		if (!audiofile::load(path, d)) {
			loadError = system::getFilename(path) + ": " + d.error;
			WARN("Splinter: cannot load %s: %s", path.c_str(), d.error.c_str());
			return false;
		}
		SampleData* s = new SampleData;
		s->left = std::move(d.left);
		s->right = std::move(d.right);
		s->sampleRate = d.sampleRate;
		s->name = system::getStem(path);
		s->analyze();
		queueSample(s);
		samplePath = path;
		sampleName = s->name;
		sampleRecorded = false;
		loadError.clear();
		lastDir = system::getDirectory(path);
		return true;
	}

	void clearSample() {
		delete pendingSample.exchange(nullptr);
		clearRequest.store(true);
		uiSample = nullptr;
		samplePath.clear();
		sampleName.clear();
		sampleRecorded = false;
		loadError.clear();
	}

	void openLoadDialog() {
		osdialog_filters* filters = osdialog_filters_parse("Audio (WAV, AIFF):wav,wave,aif,aiff,aifc");
		DEFER({
			osdialog_filters_free(filters);
		});
		char* pathC = osdialog_file(OSDIALOG_OPEN, lastDir.empty() ? NULL : lastDir.c_str(), NULL, filters);
		if (!pathC)
			return;
		std::string path = pathC;
		std::free(pathC);
		loadSampleFile(path);
	}

	void saveRecording() {
		if (!uiSample || !uiSample->recorded)
			return;
		try {
			std::string dir = createPatchStorageDirectory();
			audiofile::writeWav(system::join(dir, RECORDING_FILE), uiSample->L(), uiSample->stereo() ? uiSample->R() : NULL, uiSample->frames, uiSample->sampleRate);
		}
		catch (Exception& e) {
			WARN("Splinter: cannot save recording: %s", e.what());
		}
	}

	// 可能抛出 Exception（模块尚未加入引擎时）
	void loadRecording() {
		std::string path = system::join(getPatchStorageDirectory(), RECORDING_FILE);
		audiofile::Decoded d;
		if (!audiofile::load(path, d)) {
			loadError = "recording missing";
			return;
		}
		SampleData* s = new SampleData;
		s->left = std::move(d.left);
		s->right = std::move(d.right);
		s->sampleRate = d.sampleRate;
		s->name = "recording";
		s->recorded = true;
		s->analyze();
		queueSample(s);
		samplePath.clear();
		sampleName = string::f("REC %.1fs", s->frames / s->sampleRate);
		sampleRecorded = true;
		loadError.clear();
	}

	// 每帧由 ModuleWidget::step() 调用
	void uiStep() {
		delete retiredSample.exchange(nullptr);

		if (recState.load() == REC_DONE) {
			SampleData* s = new SampleData;
			s->left.assign(recL.get(), recL.get() + recPos);
			if (recStereo)
				s->right.assign(recR.get(), recR.get() + recPos);
			s->sampleRate = recSampleRate;
			s->name = "recording";
			s->recorded = true;
			s->analyze();
			recState.store(REC_IDLE);
			queueSample(s);
			samplePath.clear();
			sampleName = string::f("REC %.1fs", s->frames / s->sampleRate);
			sampleRecorded = true;
			loadError.clear();
			// 立即写入补丁存储，崩溃恢复/自动保存也不会丢
			saveRecording();
		}
	}

	void onAdd(const AddEvent& e) override {
		if (deferredRecordingLoad) {
			deferredRecordingLoad = false;
			try {
				loadRecording();
			}
			catch (Exception& ex) {
				WARN("Splinter: %s", ex.what());
			}
		}
	}

	void onSave(const SaveEvent& e) override {
		if (sampleRecorded)
			saveRecording();
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "path", json_string(samplePath.c_str()));
		json_object_set_new(rootJ, "recorded", json_boolean(sampleRecorded));
		json_object_set_new(rootJ, "recLength", json_integer(recLengthMode));
		json_object_set_new(rootJ, "stepDivision", json_integer(stepDivision));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* recLengthJ = json_object_get(rootJ, "recLength");
		if (recLengthJ)
			recLengthMode = clamp((int) json_integer_value(recLengthJ), 0, 3);
		json_t* divJ = json_object_get(rootJ, "stepDivision");
		if (divJ)
			stepDivision = clamp((int) json_integer_value(divJ), 0, 3);

		bool recorded = json_is_true(json_object_get(rootJ, "recorded"));
		json_t* pathJ = json_object_get(rootJ, "path");
		std::string path = pathJ ? json_string_value(pathJ) : "";

		if (recorded) {
			// 撤销/重做时已经是同一段录音，不必重新读
			if (sampleRecorded && uiSample)
				return;
			try {
				loadRecording();
			}
			catch (Exception& e) {
				// 补丁载入时模块还没加入引擎，等 onAdd 再读
				deferredRecordingLoad = true;
			}
		}
		else if (!path.empty()) {
			if (path == samplePath && uiSample)
				return;
			if (!loadSampleFile(path)) {
				// 保留路径，这样再次保存补丁时不会丢掉引用
				samplePath = path;
				sampleName = system::getStem(path);
			}
		}
		else if (uiSample) {
			clearSample();
		}
	}
};

std::string SliceQuantity::getDisplayValueString() {
	Splinter* m = dynamic_cast<Splinter*>(module);
	int n = m ? m->sliceCount() : NUM_STEPS;
	int v = (int) getValue();
	if (v > n)
		return string::f("%d → %d / %d", v, wrapSlice(v - 1, n) + 1, n);
	return string::f("%d / %d", v, n);
}

void SliceQuantity::setDisplayValueString(std::string s) {
	int idx = std::atoi(s.c_str());
	if (idx >= 1)
		setValue(clamp(idx, 1, MAX_SLICES));
}

// ======================= 界面 =======================

namespace {

// 黑 / 粉 / 白（和 tools/make_panel.py 保持一致）
// 热粉 = 主色（音序、切片），白 = 打击乐模式，浅粉 = 次要数值
const NVGcolor COL_INK = nvgRGB(0x0d, 0x0b, 0x0e);
const NVGcolor COL_WHITE = nvgRGB(0xff, 0xf6, 0xfa);
const NVGcolor COL_PINK = nvgRGB(0xff, 0x5c, 0x9e);
const NVGcolor COL_PINK_SOFT = nvgRGB(0xff, 0xb1, 0xcf);
const NVGcolor COL_LABEL = nvgRGB(0xc9, 0xb9, 0xc3);
const NVGcolor COL_DIM = nvgRGB(0x86, 0x77, 0x81);
const NVGcolor COL_FAINT = nvgRGB(0x45, 0x3a, 0x43);
const NVGcolor COL_RED = nvgRGB(0xff, 0x3b, 0x55);
const NVGcolor COL_PAD = nvgRGB(0x1f, 0x1a, 0x21);
const NVGcolor COL_PAD_EDGE = nvgRGB(0x38, 0x2e, 0x37);
const NVGcolor COL_TRACK = nvgRGB(0x2e, 0x26, 0x2f);
const NVGcolor COL_HAIR = nvgRGB(0x2a, 0x22, 0x29);

std::shared_ptr<window::Font> monoFont() {
	return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

std::shared_ptr<window::Font> labelFont() {
	return APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
}

std::shared_ptr<window::Font> cjkFont() {
	return APP->window->loadFont(asset::system("res/fonts/NotoSansSC-Medium.otf"));
}

void drawPadLabel(NVGcontext* vg, Vec size, const std::string& label, float sizeMm, NVGcolor color) {
	std::shared_ptr<window::Font> font = labelFont();
	if (!font)
		return;
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, mm2px(sizeMm));
	nvgTextLetterSpacing(vg, mm2px(0.12f));
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, color);
	nvgText(vg, size.x / 2, size.y / 2 + mm2px(0.1f), label.c_str(), NULL);
}

// 熄灭状态：深色药丸
void drawPadBase(NVGcontext* vg, Vec size, const std::string& label, float labelMm) {
	float r = std::min(size.x, size.y) * 0.5f;
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 0, 0, size.x, size.y, r);
	nvgFillColor(vg, COL_PAD);
	nvgFill(vg);
	nvgStrokeWidth(vg, mm2px(0.22f));
	nvgStrokeColor(vg, COL_PAD_EDGE);
	nvgStroke(vg);
	if (!label.empty())
		drawPadLabel(vg, size, label, labelMm, COL_DIM);
}

// 点亮状态：实心色块 + 光晕（画在发光层，暗房间里也看得见）
void drawPadLit(NVGcontext* vg, Vec size, float lit, NVGcolor color, const std::string& label, float labelMm) {
	if (lit <= 0.01f)
		return;
	float r = std::min(size.x, size.y) * 0.5f;
	float g = mm2px(1.8f);
	nvgBeginPath(vg);
	nvgRect(vg, -g, -g, size.x + 2 * g, size.y + 2 * g);
	nvgFillPaint(vg, nvgBoxGradient(vg, 0, 0, size.x, size.y, r, g, nvgTransRGBAf(color, 0.32f * lit), nvgTransRGBAf(color, 0.f)));
	nvgFill(vg);
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 0, 0, size.x, size.y, r);
	nvgFillColor(vg, nvgTransRGBAf(color, lit));
	nvgFill(vg);
	if (!label.empty() && lit > 0.5f)
		drawPadLabel(vg, size, label, labelMm, COL_INK);
}

// 小爱心（和标题前的碎心同一个造型）。cx/cy 是中心，w 是宽度。
void drawHeart(NVGcontext* vg, float cx, float cy, float w, NVGcolor color) {
	float k = w / 10.f;
	float x0 = cx - 5.f * k;
	float y0 = cy - 4.5f * k;
	auto bez = [&](float x1, float y1, float x2, float y2, float x, float y) {
		nvgBezierTo(vg, x0 + x1 * k, y0 + y1 * k, x0 + x2 * k, y0 + y2 * k, x0 + x * k, y0 + y * k);
	};
	nvgBeginPath(vg);
	nvgMoveTo(vg, x0 + 5.f * k, y0 + 9.f * k);
	bez(2, 7, 0, 5, 0, 3);
	bez(0, 1.3f, 1.3f, 0, 3, 0);
	bez(4, 0, 4.6f, 0.6f, 5, 1.2f);
	bez(5.4f, 0.6f, 6, 0, 7, 0);
	bez(8.7f, 0, 10, 1.3f, 10, 3);
	bez(10, 5, 8, 7, 5, 9);
	nvgClosePath(vg);
	nvgFillColor(vg, color);
	nvgFill(vg);
}

} // namespace

// 带发光数值弧线的旋钮：静态底座（bg）+ 旋转指针（sw）+ nanovg 画的轨道和数值弧
struct PBKnob : app::SvgKnob {
	widget::SvgWidget* bg;
	NVGcolor arcColor = COL_PINK;
	bool bipolar = false;
	// 模块浏览器预览时没有参数，用这个值（0..1）画弧线
	float previewValue = -1.f;
	float trackRadius = 0.f;
	float trackWidth = 0.f;

	PBKnob() {
		minAngle = -0.78f * M_PI;
		maxAngle = 0.78f * M_PI;
		bg = new widget::SvgWidget;
		fb->addChildBelow(bg, tw);
	}

	void setup(const std::string& name, float bodyMm, float trackRadiusMm, float trackWidthMm) {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob-" + name + "-fg.svg")));
		bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knob-" + name + "-bg.svg")));
		float b = mm2px(bodyMm);
		shadow->box.size = Vec(b, b);
		shadow->box.pos = box.size.div(2).minus(Vec(b / 2, b / 2)).plus(Vec(0, b * 0.1f));
		trackRadius = mm2px(trackRadiusMm);
		trackWidth = mm2px(trackWidthMm);
	}

	void arc(NVGcontext* vg, float a0, float a1, NVGcolor color, float width) {
		if (a1 < a0)
			std::swap(a0, a1);
		Vec c = box.size.div(2);
		nvgBeginPath(vg);
		nvgArc(vg, c.x, c.y, trackRadius, a0 - M_PI / 2, a1 - M_PI / 2, NVG_CW);
		nvgStrokeColor(vg, color);
		nvgStrokeWidth(vg, width);
		nvgLineCap(vg, NVG_ROUND);
		nvgStroke(vg);
	}

	void draw(const DrawArgs& args) override {
		arc(args.vg, minAngle, maxAngle, COL_TRACK, trackWidth);
		SvgKnob::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			ParamQuantity* pq = getParamQuantity();
			float v = pq ? pq->getScaledValue() : previewValue;
			if (v >= 0.f) {
				float a0 = bipolar ? 0.5f * (minAngle + maxAngle) : minAngle;
				float a1 = minAngle + v * (maxAngle - minAngle);
				if (std::fabs(a1 - a0) > 0.02f) {
					arc(args.vg, a0, a1, nvgTransRGBAf(arcColor, 0.16f), trackWidth * 2.8f);
					arc(args.vg, a0, a1, arcColor, trackWidth);
				}
			}
		}
		SvgKnob::drawLayer(args, layer);
	}
};

struct StepKnob : PBKnob {
	StepKnob() {
		setup("step", 4.9f, 3.1f, 0.48f);
	}
};

struct MidKnob : PBKnob {
	MidKnob() {
		setup("mid", 7.f, 4.3f, 0.6f);
	}
};

struct BigKnob : PBKnob {
	BigKnob() {
		setup("big", 9.4f, 5.6f, 0.7f);
	}
};

// 自绘的药丸开关。同一 paintGroup 的开关可以按住拖过去“刷”成同一个状态。
struct PadSwitch : app::Switch {
	NVGcolor color = COL_PINK;
	std::string label;
	float labelMm = 1.9f;
	int lightId = -1;
	int paintGroup = 0;
	float previewLit = 0.f;

	float lit() {
		if (!module)
			return previewLit;
		if (lightId >= 0)
			return module->lights[lightId].getBrightness();
		ParamQuantity* pq = getParamQuantity();
		return pq ? pq->getScaledValue() : 0.f;
	}

	void draw(const DrawArgs& args) override {
		drawPadBase(args.vg, box.size, label, labelMm);
		Switch::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			drawPadLit(args.vg, box.size, lit(), color, label, labelMm);
		Switch::drawLayer(args, layer);
	}

	void onDragEnter(const DragEnterEvent& e) override {
		PadSwitch* origin = dynamic_cast<PadSwitch*>(e.origin);
		if (origin && origin != this && paintGroup > 0 && origin->paintGroup == paintGroup) {
			ParamQuantity* pq = getParamQuantity();
			ParamQuantity* opq = origin->getParamQuantity();
			if (pq && opq && pq->getValue() != opq->getValue()) {
				history::ParamChange* h = new history::ParamChange;
				h->name = "paint steps";
				h->moduleId = module->id;
				h->paramId = paramId;
				h->oldValue = pq->getValue();
				h->newValue = opq->getValue();
				pq->setValue(opq->getValue());
				APP->history->push(h);
			}
		}
		Switch::onDragEnter(e);
	}
};

// 不绑定参数的按钮（LOAD）
struct PadButton : widget::OpaqueWidget {
	NVGcolor color = COL_PINK;
	std::string label;
	float labelMm = 1.9f;
	bool pressed = false;
	std::function<void()> action;

	void draw(const DrawArgs& args) override {
		drawPadBase(args.vg, box.size, label, labelMm);
	}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1 && pressed)
			drawPadLit(args.vg, box.size, 1.f, color, label, labelMm);
		OpaqueWidget::drawLayer(args, layer);
	}
	void onDragStart(const DragStartEvent& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT)
			pressed = true;
	}
	void onDragEnd(const DragEndEvent& e) override {
		pressed = false;
	}
	void onDragDrop(const DragDropEvent& e) override {
		if (e.origin == this && action)
			action();
	}
};

// 面板上的静态文字（Rack 的 SVG 渲染器不支持 <text>，所以在这里画，并用帧缓冲缓存）
struct PanelLabels : widget::Widget {
	enum Face { BOLD, MONO, CJK };
	struct Label {
		float x, y, size, spacing;
		NVGcolor color;
		int align;
		std::string text;
		Face face;
		// true：x 是相对上一段文字末尾的间距（用来拼中英文混排的标题）
		bool after;
	};
	std::vector<Label> labels;

	void add(float x, float y, std::string text, NVGcolor color = COL_LABEL, float size = 2.1f, int align = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, float spacing = 0.06f, Face face = BOLD) {
		labels.push_back({x, y, size, spacing, color, align, text, face, false});
	}

	void addAfter(float gap, std::string text, NVGcolor color, float size, float spacing, Face face = BOLD) {
		const Label& prev = labels.back();
		labels.push_back({gap, prev.y, size, spacing, color, prev.align, text, face, true});
	}

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> fonts[3] = {labelFont(), monoFont(), cjkFont()};
		float endX = 0.f;
		for (const Label& l : labels) {
			std::shared_ptr<window::Font> f = fonts[l.face];
			if (!f)
				continue;
			nvgFontFaceId(args.vg, f->handle);
			nvgFontSize(args.vg, mm2px(l.size));
			nvgTextLetterSpacing(args.vg, mm2px(l.spacing));
			nvgFillColor(args.vg, l.color);
			nvgTextAlign(args.vg, l.align);
			float x = l.after ? endX + mm2px(l.x) : mm2px(l.x);
			endX = nvgText(args.vg, x, mm2px(l.y), l.text.c_str(), NULL);
		}
	}
};

// 左上：BPM / 当前步 / 采样名 / 切片信息
struct InfoDisplay : widget::Widget {
	Splinter* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		std::shared_ptr<window::Font> digits = APP->window->loadFont(asset::system("res/fonts/DSEG7ClassicMini-Bold.ttf"));
		std::shared_ptr<window::Font> mono = monoFont();
		std::shared_ptr<window::Font> bold = labelFont();
		if (!digits || !mono || !bold)
			return;
		float w = box.size.x;
		float bpm = module ? module->uiBpm : 120.f;
		bool ext = module && module->externalClock;

		// 速度数码管
		nvgFontFaceId(vg, digits->handle);
		nvgFontSize(vg, mm2px(6.4f));
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgTransRGBAf(COL_PINK, 0.07f));
		nvgText(vg, w - mm2px(2.f), mm2px(5.6f), "888.8", NULL);
		nvgFillColor(vg, COL_PINK);
		std::string bpmText = string::f("%5.1f", clamp(bpm, 0.f, 999.9f));
		nvgText(vg, w - mm2px(2.f), mm2px(5.6f), bpmText.c_str(), NULL);

		nvgFontFaceId(vg, bold->handle);
		nvgFontSize(vg, mm2px(1.7f));
		nvgTextLetterSpacing(vg, mm2px(0.15f));
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, ext ? COL_PINK_SOFT : COL_DIM);
		nvgText(vg, mm2px(2.f), mm2px(3.2f), ext ? "EXT CLOCK" : "TEMPO", NULL);

		nvgFontFaceId(vg, mono->handle);
		nvgTextLetterSpacing(vg, 0.f);
		nvgFontSize(vg, mm2px(2.f));
		int step = module ? module->currentStep : -1;
		std::string st = step >= 0 ? string::f("STEP %02d/%02d", step + 1, (int) module->params[Splinter::LENGTH_PARAM].getValue()) : "STEP --";
		nvgFillColor(vg, step >= 0 ? COL_LABEL : COL_DIM);
		nvgText(vg, mm2px(2.f), mm2px(7.4f), st.c_str(), NULL);

		// 分隔线
		nvgBeginPath(vg);
		nvgRect(vg, mm2px(2.f), mm2px(10.4f), w - mm2px(4.f), mm2px(0.2f));
		nvgFillColor(vg, COL_HAIR);
		nvgFill(vg);

		// 采样名
		std::string name = "NO SAMPLE";
		NVGcolor nameCol = COL_DIM;
		if (module) {
			if (!module->loadError.empty()) {
				name = "LOAD ERROR";
				nameCol = COL_RED;
			}
			else if (!module->sampleName.empty()) {
				name = module->sampleName;
				nameCol = COL_WHITE;
				if (!module->uiSample) {
					name = "MISSING " + name;
					nameCol = COL_RED;
				}
			}
		}
		if (name.size() > 22)
			name = name.substr(0, 21) + "~";
		nvgFillColor(vg, nameCol);
		nvgFontSize(vg, mm2px(2.2f));
		nvgText(vg, mm2px(2.f), mm2px(12.9f), name.c_str(), NULL);

		// 切片 / 录音状态
		std::string status;
		NVGcolor statusCol = COL_DIM;
		int rs = module ? module->recState.load(std::memory_order_relaxed) : REC_IDLE;
		if (rs == REC_RUNNING) {
			status = string::f("REC %.1fs", module->recPos / module->recSampleRate);
			statusCol = COL_RED;
		}
		else if (rs == REC_ARMED) {
			status = "REC ARMED > STEP 1";
			statusCol = COL_RED;
		}
		else {
			int n = module ? module->numSlices : NUM_STEPS;
			bool trans = module && module->params[Splinter::SLICEMODE_PARAM].getValue() > 0.5f;
			status = string::f("%d SLICES / %s", n, trans ? "TRANSIENT" : "GRID");
		}
		nvgFillColor(vg, statusCol);
		nvgFontSize(vg, mm2px(1.9f));
		nvgText(vg, mm2px(2.f), mm2px(15.6f), status.c_str(), NULL);
	}
};

// 右上：波形、切片线、当前播放切片与播放头
struct WaveDisplay : widget::Widget {
	Splinter* module = nullptr;
	uint32_t cachedSerial = 0xFFFFFFFF;
	bool hasPeaks = false;
	int frames = 0;
	float peakMin[SampleData::PEAKS];
	float peakMax[SampleData::PEAKS];
	float norm = 1.f;

	void step() override {
		Widget::step();
		if (!module)
			return;
		uint32_t serial = module->sampleSerial.load();
		if (serial == cachedSerial)
			return;
		cachedSerial = serial;
		SampleData* s = module->current.load();
		hasPeaks = s && s->frames > 0;
		if (!hasPeaks)
			return;
		frames = s->frames;
		float mx = 1e-4f;
		for (int i = 0; i < SampleData::PEAKS; i++) {
			peakMin[i] = s->peakMin[i];
			peakMax[i] = s->peakMax[i];
			mx = std::max(mx, std::max(std::fabs(peakMin[i]), std::fabs(peakMax[i])));
		}
		norm = 1.f / mx;
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		std::shared_ptr<window::Font> mono = monoFont();
		std::shared_ptr<window::Font> bold = labelFont();
		if (!mono || !bold)
			return;
		float w = box.size.x;
		float h = box.size.y;
		float top = mm2px(4.f);
		float bottom = h - mm2px(1.4f);
		float mid = 0.5f * (top + bottom);
		float amp = 0.5f * (bottom - top);

		nvgSave(vg);
		nvgScissor(vg, 0, 0, w, h);

		// 中线
		nvgBeginPath(vg);
		nvgRect(vg, 0, mid, w, mm2px(0.15f));
		nvgFillColor(vg, nvgRGB(0x1f, 0x1a, 0x20));
		nvgFill(vg);

		if (!module || !hasPeaks) {
			nvgFontFaceId(vg, bold->handle);
			nvgFontSize(vg, mm2px(2.6f));
			nvgTextLetterSpacing(vg, mm2px(0.3f));
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			drawHeart(vg, w / 2, h / 2 - mm2px(7.f), mm2px(2.6f), COL_PINK);
			nvgFillColor(vg, COL_WHITE);
			nvgText(vg, w / 2, h / 2 - mm2px(2.2f), "drop a wav / aiff here", NULL);
			nvgFontFaceId(vg, mono->handle);
			nvgTextLetterSpacing(vg, 0.f);
			nvgFontSize(vg, mm2px(2.f));
			nvgFillColor(vg, COL_DIM);
			nvgText(vg, w / 2, h / 2 + mm2px(2.2f), "LOAD button / right-click > Load sample / patch audio into IN and press REC", NULL);
			drawRecOverlay(vg, w);
			nvgRestore(vg);
			return;
		}

		int n = clamp(module->numSlices, 0, MAX_SLICES);
		auto xOf = [&](int frame) {
			return (float) frame / std::max(1, frames) * w;
		};

		// 正在播放的切片高亮
		int playing = module->uiPlaySlice;
		if (playing >= 0 && playing < n) {
			float x0 = xOf(module->sliceStart[playing]);
			float x1 = xOf(module->sliceStart[playing + 1]);
			nvgBeginPath(vg);
			nvgRect(vg, x0, 0, std::max(1.f, x1 - x0), h);
			nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, h, nvgTransRGBAf(COL_PINK, 0.22f), nvgTransRGBAf(COL_PINK, 0.04f)));
			nvgFill(vg);
		}

		// 波形
		int cols = std::max(2, (int) w);
		nvgBeginPath(vg);
		for (int c = 0; c < cols; c++) {
			int b = c * SampleData::PEAKS / cols;
			int e = std::max(b + 1, (c + 1) * SampleData::PEAKS / cols);
			float mx = -1e9f;
			for (int i = b; i < e && i < SampleData::PEAKS; i++)
				mx = std::max(mx, peakMax[i]);
			float y = mid - clamp(mx * norm, -1.f, 1.f) * amp;
			float x = (float) c / (cols - 1) * w;
			if (c == 0)
				nvgMoveTo(vg, x, y);
			else
				nvgLineTo(vg, x, y);
		}
		for (int c = cols - 1; c >= 0; c--) {
			int b = c * SampleData::PEAKS / cols;
			int e = std::max(b + 1, (c + 1) * SampleData::PEAKS / cols);
			float mn = 1e9f;
			for (int i = b; i < e && i < SampleData::PEAKS; i++)
				mn = std::min(mn, peakMin[i]);
			float y = mid - clamp(mn * norm, -1.f, 1.f) * amp;
			float x = (float) c / (cols - 1) * w;
			nvgLineTo(vg, x, y);
		}
		nvgClosePath(vg);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, top, 0, bottom, nvgTransRGBAf(COL_WHITE, 0.92f), nvgTransRGBAf(COL_PINK_SOFT, 0.5f)));
		nvgFill(vg);

		// 切片线和编号
		nvgFontFaceId(vg, mono->handle);
		nvgFontSize(vg, mm2px(1.8f));
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
		int labelEvery = n <= 32 ? 1 : 2;
		for (int k = 0; k < n; k++) {
			float x = xOf(module->sliceStart[k]);
			bool isPlaying = k == playing;
			nvgBeginPath(vg);
			nvgRect(vg, x, 0, isPlaying ? 1.2f : 0.7f, h);
			nvgFillColor(vg, isPlaying ? COL_PINK : nvgTransRGBAf(COL_PINK, 0.32f));
			nvgFill(vg);
			if (k % labelEvery == 0) {
				float next = xOf(module->sliceStart[k + 1]);
				// 切片太窄就不写编号，避免挤在一起
				if (next - x > mm2px(2.6f) || isPlaying) {
					nvgFillColor(vg, isPlaying ? COL_PINK : COL_DIM);
					std::string t = string::f("%d", k + 1);
					nvgText(vg, x + mm2px(0.5f), mm2px(0.6f), t.c_str(), NULL);
				}
			}
		}

		// 播放头
		float pp = module->uiPlayPos;
		if (pp >= 0.f) {
			float x = pp * w;
			nvgBeginPath(vg);
			nvgRect(vg, x - 0.7f, 0, 1.4f, h);
			nvgFillColor(vg, COL_WHITE);
			nvgFill(vg);
		}

		drawRecOverlay(vg, w);
		nvgRestore(vg);
	}

	void drawRecOverlay(NVGcontext* vg, float w) {
		if (!module)
			return;
		int rs = module->recState.load(std::memory_order_relaxed);
		if (rs != REC_RUNNING && rs != REC_ARMED)
			return;
		std::shared_ptr<window::Font> mono = monoFont();
		if (!mono)
			return;
		nvgFontFaceId(vg, mono->handle);
		nvgFontSize(vg, mm2px(2.3f));
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
		nvgFillColor(vg, COL_RED);
		std::string t = rs == REC_RUNNING ? string::f("REC %.1fs", module->recPos / module->recSampleRate) : "REC ARMED";
		nvgText(vg, w - mm2px(1.f), mm2px(0.6f), t.c_str(), NULL);
	}
};

// 步进区底层：当前步的竖向高亮条；发光层：步号和每步的数值读数
struct StepBackdrop : widget::Widget {
	Splinter* module = nullptr;

	void draw(const DrawArgs& args) override {
		if (!module || module->currentStep < 0)
			return;
		NVGcontext* vg = args.vg;
		float x = mm2px(stepX(module->currentStep) - STEP_COL_W / 2 + 0.25f);
		float wCol = mm2px(STEP_COL_W - 0.5f);
		float y0 = mm2px(STEP_AREA_TOP + 0.5f);
		float y1 = mm2px(STEP_AREA_BOTTOM - 0.5f);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x, y0, wCol, y1 - y0, mm2px(0.9f));
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, y0, 0, y1, nvgTransRGBAf(COL_PINK, 0.13f), nvgTransRGBAf(COL_PINK, 0.03f)));
		nvgFill(vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			Widget::drawLayer(args, layer);
			return;
		}
		NVGcontext* vg = args.vg;
		std::shared_ptr<window::Font> mono = monoFont();
		if (!mono)
			return;
		nvgFontFaceId(vg, mono->handle);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

		int cur = module ? module->currentStep : -1;
		int n = module ? module->sliceCount() : NUM_STEPS;
		for (int i = 0; i < NUM_STEPS; i++) {
			float x = mm2px(stepX(i));

			// 步号：当前步粉色并在下面挂一颗小心，每组第一步亮一些
			nvgFontSize(vg, mm2px(2.f));
			nvgFillColor(vg, i == cur ? COL_PINK : (i % 4 == 0) ? COL_LABEL : COL_DIM);
			std::string t = string::f("%d", i + 1);
			nvgText(vg, x, mm2px(Y_NUM), t.c_str(), NULL);
			if (i == cur)
				drawHeart(vg, x, mm2px(Y_NUM + 2.1f), mm2px(1.7f), COL_PINK);

			int slot = module ? (int) module->params[Splinter::SLICE_PARAM + i].getValue() : i + 1;
			float pitch = module ? module->params[Splinter::PITCH_PARAM + i].getValue() : 0.f;
			int ratchet = module ? (int) module->params[Splinter::RATCHET_PARAM + i].getValue() : 1;
			bool on = module ? module->params[Splinter::ON_PARAM + i].getValue() > 0.5f : true;

			nvgFontSize(vg, mm2px(1.85f));
			int idx = wrapSlice(slot - 1, n);
			t = string::f("%d", idx + 1);
			nvgFillColor(vg, on ? COL_PINK : COL_FAINT);
			nvgText(vg, x, mm2px(Y_SLICE_TXT), t.c_str(), NULL);

			int p = (int) std::round(pitch);
			t = p == 0 ? "0" : string::f("%+d", p);
			nvgFillColor(vg, p == 0 ? COL_FAINT : COL_WHITE);
			nvgText(vg, x, mm2px(Y_PITCH_TXT), t.c_str(), NULL);

			t = string::f("x%d", ratchet);
			nvgFillColor(vg, ratchet > 1 ? COL_WHITE : COL_FAINT);
			nvgText(vg, x, mm2px(Y_RATCHET_TXT), t.c_str(), NULL);
		}
	}
};

// 序列长度以外的步压暗（画在所有步进控件之上）
struct LengthShade : widget::TransparentWidget {
	Splinter* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1 || !module) {
			Widget::drawLayer(args, layer);
			return;
		}
		int len = (int) module->params[Splinter::LENGTH_PARAM].getValue();
		if (len >= NUM_STEPS)
			return;
		NVGcontext* vg = args.vg;
		float x0 = mm2px(stepX(len) - STEP_COL_W / 2);
		float x1 = mm2px(stepX(NUM_STEPS - 1) + STEP_COL_W / 2);
		nvgBeginPath(vg);
		nvgRect(vg, x0, mm2px(STEP_AREA_TOP), x1 - x0, mm2px(STEP_AREA_BOTTOM - STEP_AREA_TOP));
		nvgFillColor(vg, nvgTransRGBAf(COL_INK, 0.62f));
		nvgFill(vg);
	}
};

struct SplinterWidget : ModuleWidget {
	// 只在模块浏览器预览（没有 module）时临时存在，用来读参数默认值
	Splinter* previewDefaults = nullptr;

	template <class TKnob>
	TKnob* addKnob(float xMm, float yMm, int paramId, NVGcolor color, bool bipolar = false) {
		TKnob* k = createParamCentered<TKnob>(mm2px(Vec(xMm, yMm)), module, paramId);
		k->arcColor = color;
		k->bipolar = bipolar;
		if (previewDefaults) {
			ParamQuantity* pq = previewDefaults->getParamQuantity(paramId);
			k->previewValue = pq ? pq->getScaledValue() : -1.f;
		}
		addParam(k);
		return k;
	}

	PadSwitch* addPad(float xMm, float yMm, Vec sizeMm, int paramId, NVGcolor color, std::string label = "", bool momentary = false) {
		PadSwitch* p = createParam<PadSwitch>(Vec(0, 0), module, paramId);
		p->box.size = mm2px(sizeMm);
		p->box.pos = mm2px(Vec(xMm, yMm)).minus(p->box.size.div(2));
		p->color = color;
		p->label = label;
		p->momentary = momentary;
		addParam(p);
		return p;
	}

	SplinterWidget(Splinter* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Splinter.svg")));
		std::unique_ptr<Splinter> defaults;
		if (!module) {
			defaults.reset(new Splinter);
			previewDefaults = defaults.get();
		}

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addLabels();

		InfoDisplay* info = createWidget<InfoDisplay>(mm2px(Vec(4.f, 6.5f)));
		info->box.size = mm2px(Vec(48.f, 18.f));
		info->module = module;
		addChild(info);

		WaveDisplay* wave = createWidget<WaveDisplay>(mm2px(Vec(59.f, 12.5f)));
		wave->box.size = mm2px(Vec(261.f, 25.5f));
		wave->module = module;
		addChild(wave);

		StepBackdrop* backdrop = createWidget<StepBackdrop>(Vec(0, 0));
		backdrop->box.size = box.size;
		backdrop->module = module;
		addChild(backdrop);

		// ---- 左侧：全局控制 ----
		const float* X = LEFT_X;
		const float* Y = LEFT_Y;
		const Vec bigPad = Vec(9.4f, 5.8f);
		addKnob<BigKnob>(X[0], Y[0], Splinter::BPM_PARAM, COL_PINK);
		addKnob<MidKnob>(X[1], Y[0], Splinter::SWING_PARAM, COL_PINK);
		addKnob<MidKnob>(X[2], Y[0], Splinter::LENGTH_PARAM, COL_PINK);
		addKnob<MidKnob>(X[3], Y[0], Splinter::LEVEL_PARAM, COL_PINK_SOFT);

		addKnob<MidKnob>(X[0], Y[1], Splinter::SLICES_PARAM, COL_PINK);
		addPad(X[1], Y[1], bigPad, Splinter::SLICEMODE_PARAM, COL_PINK, "trans");
		addKnob<MidKnob>(X[2], Y[1], Splinter::BITS_PARAM, COL_PINK);
		addKnob<MidKnob>(X[3], Y[1], Splinter::RATE_PARAM, COL_PINK);

		addKnob<MidKnob>(X[0], Y[2], Splinter::ATTACK_PARAM, COL_WHITE);
		addKnob<MidKnob>(X[1], Y[2], Splinter::DECAY_PARAM, COL_WHITE);
		addKnob<MidKnob>(X[2], Y[2], Splinter::SUSTAIN_PARAM, COL_WHITE);
		addKnob<MidKnob>(X[3], Y[2], Splinter::RELEASE_PARAM, COL_WHITE);

		addKnob<MidKnob>(X[0], Y[3], Splinter::PUNCH_PARAM, COL_WHITE);
		addKnob<MidKnob>(X[1], Y[3], Splinter::DRIVE_PARAM, COL_WHITE);
		addPad(X[2], Y[3], bigPad, Splinter::RUN_PARAM, COL_PINK, "run")->previewLit = 1.f;
		addPad(X[3], Y[3], bigPad, Splinter::RESET_PARAM, COL_PINK_SOFT, "reset", true);

		PadButton* load = createWidget<PadButton>(Vec(0, 0));
		load->box.size = mm2px(bigPad);
		load->box.pos = mm2px(Vec(X[0], Y[4])).minus(load->box.size.div(2));
		load->label = "load";
		load->action = [=]() {
			if (module)
				module->openLoadDialog();
		};
		addChild(load);
		PadSwitch* rec = addPad(X[1], Y[4], bigPad, Splinter::REC_PARAM, COL_RED, "rec", true);
		rec->lightId = Splinter::REC_LIGHT;
		addInput(createInputCentered<DarkPJ301MPort>(mm2px(Vec(X[2], Y[4])), module, Splinter::IN_L_INPUT));
		addInput(createInputCentered<DarkPJ301MPort>(mm2px(Vec(X[3], Y[4])), module, Splinter::IN_R_INPUT));

		addInput(createInputCentered<DarkPJ301MPort>(mm2px(Vec(X[0], Y[5])), module, Splinter::CLOCK_INPUT));
		addInput(createInputCentered<DarkPJ301MPort>(mm2px(Vec(X[1], Y[5])), module, Splinter::RESET_INPUT));
		addInput(createInputCentered<DarkPJ301MPort>(mm2px(Vec(X[2], Y[5])), module, Splinter::RUN_INPUT));
		addInput(createInputCentered<DarkPJ301MPort>(mm2px(Vec(X[3], Y[5])), module, Splinter::REC_INPUT));

		addInput(createInputCentered<DarkPJ301MPort>(mm2px(Vec(X[0], Y[6])), module, Splinter::SLICE_CV_INPUT));
		addOutput(createOutputCentered<DarkPJ301MPort>(mm2px(Vec(X[1], Y[6])), module, Splinter::TRIG_OUTPUT));
		addOutput(createOutputCentered<DarkPJ301MPort>(mm2px(Vec(X[2], Y[6])), module, Splinter::OUT_L_OUTPUT));
		addOutput(createOutputCentered<DarkPJ301MPort>(mm2px(Vec(X[3], Y[6])), module, Splinter::OUT_R_OUTPUT));

		// ---- 右侧：32 步 ----
		const Vec stepPad = Vec(6.f, 4.2f);
		for (int i = 0; i < NUM_STEPS; i++) {
			float x = stepX(i);
			PadSwitch* on = addPad(x, Y_ON, stepPad, Splinter::ON_PARAM + i, COL_PINK);
			on->paintGroup = 1;
			on->previewLit = 1.f;
			addKnob<StepKnob>(x, Y_SLICE, Splinter::SLICE_PARAM + i, COL_PINK);
			addKnob<StepKnob>(x, Y_PITCH, Splinter::PITCH_PARAM + i, COL_PINK_SOFT, true);
			addKnob<StepKnob>(x, Y_VOL, Splinter::VOL_PARAM + i, COL_PINK);
			addKnob<StepKnob>(x, Y_RATCHET, Splinter::RATCHET_PARAM + i, COL_PINK_SOFT);
			PadSwitch* perc = addPad(x, Y_MODE, stepPad, Splinter::MODE_PARAM + i, COL_WHITE);
			perc->paintGroup = 2;
		}

		LengthShade* shade = createWidget<LengthShade>(Vec(0, 0));
		shade->box.size = box.size;
		shade->module = module;
		addChild(shade);
		previewDefaults = nullptr;
	}

	void addLabels() {
		PanelLabels* labels = new PanelLabels;
		labels->box.size = box.size;
		const float* X = LEFT_X;
		const float* Y = LEFT_Y;
		const int left = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;

		// 顶栏：碎心（面板 SVG 里）+ 中英文标题，三段共用一条基线
		const int base = NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE;
		labels->add(66.f, 7.6f, "turn everything into a", COL_WHITE, 4.1f, base, 0.1f);
		labels->addAfter(1.6f, "碎了吧唧的鼓", COL_PINK, 4.4f, 0.3f, PanelLabels::CJK);
		labels->addAfter(4.f, "32-step slice sampler", COL_DIM, 2.f, 0.12f);

		// 左侧：控件名（按钮的名字写在按钮里）
		const char* names[7][4] = {
			{"bpm", "swing", "steps", "level"},
			{"slices", "", "bits", "rate"},
			{"atk", "dec", "sus", "rel"},
			{"punch", "drive", "", ""},
			{"", "", "in l", "in r"},
			{"clock", "reset", "run", "rec"},
			{"slc cv", "trig", "out l", "out r"},
		};
		for (int r = 0; r < 7; r++) {
			for (int c = 0; c < 4; c++) {
				if (!names[r][c][0])
					continue;
				NVGcolor col = COL_LABEL;
				if (r == 1)
					col = COL_PINK;
				if (r == 2 || (r == 3 && c < 2))
					col = COL_WHITE;
				float dy = (r == 0 && c == 0) ? 7.7f : (r >= 4 ? 6.3f : 6.6f);
				labels->add(X[c], Y[r] - dy, names[r][c], col);
			}
		}
		// 右侧：每一行的名字
		const float rowX = 59.f;
		labels->add(rowX, Y_NUM, "step", COL_DIM, 2.f, left);
		labels->add(rowX, Y_ON, "on", COL_PINK, 2.f, left);
		labels->add(rowX, Y_SLICE, "slice", COL_PINK, 2.f, left);
		labels->add(rowX, Y_PITCH, "pitch", COL_PINK_SOFT, 2.f, left);
		labels->add(rowX, Y_VOL, "level", COL_PINK, 2.f, left);
		labels->add(rowX, Y_RATCHET, "ratch", COL_PINK_SOFT, 2.f, left);
		labels->add(rowX, Y_MODE, "perc", COL_WHITE, 2.f, left);

		widget::FramebufferWidget* fb = new widget::FramebufferWidget;
		fb->box.size = box.size;
		fb->addChild(labels);
		addChild(fb);
	}

	void step() override {
		ModuleWidget::step();
		Splinter* m = getModule<Splinter>();
		if (m)
			m->uiStep();
	}

	void onPathDrop(const PathDropEvent& e) override {
		Splinter* m = getModule<Splinter>();
		if (!m || e.paths.empty()) {
			ModuleWidget::onPathDrop(e);
			return;
		}
		m->loadSampleFile(e.paths[0]);
		e.consume(this);
	}

	void pushHistory(const std::string& name, std::function<void()> action) {
		Splinter* m = getModule<Splinter>();
		json_t* oldJ = m->toJson();
		action();
		history::ModuleChange* h = new history::ModuleChange;
		h->name = name;
		h->moduleId = m->id;
		h->oldModuleJ = oldJ;
		h->newModuleJ = m->toJson();
		APP->history->push(h);
	}

	void appendContextMenu(Menu* menu) override {
		Splinter* m = getModule<Splinter>();
		if (!m)
			return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("采样 Sample"));
		menu->addChild(createMenuItem("载入音频… Load sample", "WAV / AIFF", [=]() {
			m->openLoadDialog();
		}));
		std::string cur = m->sampleName.empty() ? "（无 none）" : m->sampleName;
		menu->addChild(createMenuLabel("当前: " + cur));
		if (!m->loadError.empty())
			menu->addChild(createMenuLabel("错误: " + m->loadError));
		menu->addChild(createMenuItem("清除采样 Clear sample", "", [=]() {
			m->clearSample();
		}, !m->uiSample && m->samplePath.empty()));
		menu->addChild(createIndexPtrSubmenuItem("录音长度 Record length", {"自由 Free（最长 30s，再按一次停止）", "1 小节 bar", "2 小节 bars", "4 小节 bars"}, &m->recLengthMode));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("音序 Sequence"));
		menu->addChild(createIndexPtrSubmenuItem("每步时值 Step length", {"1/8", "1/16", "1/16T 三连音", "1/32"}, &m->stepDivision));
		menu->addChild(createMenuItem("随机切片 Randomize slices", "", [=]() {
			pushHistory("randomize slices", [=]() {
				m->randomizeSlices();
			});
		}));
		menu->addChild(createMenuItem("切片按顺序 Slices in order", "", [=]() {
			pushHistory("slices in order", [=]() {
				m->resetSliceOrder();
			});
		}));
		menu->addChild(createMenuLabel("Ctrl/Cmd+R = IDM 随机（只改每步参数）"));
		menu->addChild(createMenuLabel("ON / PERC 按住拖动可以连续刷多步"));
	}
};

Model* modelSplinter = createModel<Splinter, SplinterWidget>("Splinter");
