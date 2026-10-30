#include "AudioFile.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <arch.hpp>
#if defined ARCH_WIN
#include <string.hpp>
#endif

namespace audiofile {

// Windows 的 fopen 不认 UTF-8 路径（中文文件夹里的采样会打不开），所以在 Windows 上改用 _wfopen
static FILE* openFile(const std::string& path, const char* mode) {
#if defined ARCH_WIN
	std::wstring wmode(mode, mode + std::strlen(mode));
	return _wfopen(rack::string::UTF8toUTF16(path).c_str(), wmode.c_str());
#else
	return std::fopen(path.c_str(), mode);
#endif
}

// 最长载入 10 分钟（按 48k 计），防止误载入超大文件占满内存
static const size_t MAX_FILE_BYTES = (size_t) 1 << 30;
static const size_t MAX_FRAMES = 48000 * 600;

static uint16_t le16(const uint8_t* p) {
	return (uint16_t) (p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t* p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static uint64_t le64(const uint8_t* p) {
	return (uint64_t) le32(p) | ((uint64_t) le32(p + 4) << 32);
}
static uint16_t be16(const uint8_t* p) {
	return (uint16_t) ((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
static uint64_t be64(const uint8_t* p) {
	return ((uint64_t) be32(p) << 32) | (uint64_t) be32(p + 4);
}

// AIFF 的 80-bit IEEE extended 采样率
static double ext80(const uint8_t* p) {
	int expon = ((p[0] & 0x7F) << 8) | p[1];
	uint64_t mant = 0;
	for (int i = 0; i < 8; i++)
		mant = (mant << 8) | p[2 + i];
	if (expon == 0 && mant == 0)
		return 0.0;
	double f = std::ldexp((double) mant, expon - 16383 - 63);
	return (p[0] & 0x80) ? -f : f;
}

struct Format {
	int channels = 0;
	// 每个采样占用的字节数（容器大小）
	int bytes = 0;
	bool isFloat = false;
	bool bigEndian = false;
	// WAV 的 8-bit 是无符号的
	bool unsigned8 = false;
};

static inline float readSample(const uint8_t* p, const Format& f) {
	if (f.isFloat) {
		if (f.bytes == 4) {
			uint32_t u = f.bigEndian ? be32(p) : le32(p);
			float v;
			std::memcpy(&v, &u, 4);
			return v;
		}
		if (f.bytes == 8) {
			uint64_t u = f.bigEndian ? be64(p) : le64(p);
			double v;
			std::memcpy(&v, &u, 8);
			return (float) v;
		}
		return 0.f;
	}
	switch (f.bytes) {
		case 1:
			return f.unsigned8 ? ((int) p[0] - 128) / 128.f : (int8_t) p[0] / 128.f;
		case 2: {
			int16_t v = (int16_t) (f.bigEndian ? be16(p) : le16(p));
			return v / 32768.f;
		}
		case 3: {
			uint32_t u = f.bigEndian
				? ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8)
				: ((uint32_t) p[2] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[0] << 8);
			return (int32_t) u / 2147483648.f;
		}
		case 4: {
			int32_t v = (int32_t) (f.bigEndian ? be32(p) : le32(p));
			return v / 2147483648.f;
		}
	}
	return 0.f;
}

static bool decode(const uint8_t* data, size_t dataBytes, const Format& f, Decoded& out) {
	if (f.channels <= 0 || f.bytes <= 0) {
		out.error = "invalid format";
		return false;
	}
	size_t frameBytes = (size_t) f.channels * f.bytes;
	size_t frames = std::min(dataBytes / frameBytes, MAX_FRAMES);
	if (frames == 0) {
		out.error = "no audio data";
		return false;
	}
	bool stereo = f.channels >= 2;
	out.left.resize(frames);
	if (stereo)
		out.right.resize(frames);
	else
		out.right.clear();
	for (size_t i = 0; i < frames; i++) {
		const uint8_t* p = data + i * frameBytes;
		float l = readSample(p, f);
		float r = stereo ? readSample(p + f.bytes, f) : l;
		// 浮点文件可能含有 NaN/Inf
		if (!std::isfinite(l))
			l = 0.f;
		if (!std::isfinite(r))
			r = 0.f;
		out.left[i] = std::max(-4.f, std::min(4.f, l));
		if (stereo)
			out.right[i] = std::max(-4.f, std::min(4.f, r));
	}
	return true;
}

static bool parseWav(const std::vector<uint8_t>& d, Decoded& out) {
	size_t pos = 12;
	int fmtTag = 0;
	int channels = 0;
	uint32_t sampleRate = 0;
	int blockAlign = 0;
	int bits = 0;
	bool haveFmt = false;
	const uint8_t* data = NULL;
	size_t dataBytes = 0;

	while (pos + 8 <= d.size()) {
		const uint8_t* id = &d[pos];
		uint32_t size = le32(&d[pos + 4]);
		pos += 8;
		size_t avail = d.size() - pos;
		const uint8_t* p = &d[pos];
		if (std::memcmp(id, "fmt ", 4) == 0 && avail >= 16) {
			fmtTag = le16(p);
			channels = le16(p + 2);
			sampleRate = le32(p + 4);
			blockAlign = le16(p + 12);
			bits = le16(p + 14);
			// WAVE_FORMAT_EXTENSIBLE：真实格式在 SubFormat GUID 的前两个字节
			if (fmtTag == 0xFFFE && size >= 40 && avail >= 26)
				fmtTag = le16(p + 24);
			haveFmt = true;
		}
		else if (std::memcmp(id, "data", 4) == 0) {
			data = p;
			// 流式写入的文件常把 data 大小写成 0 或 0xFFFFFFFF
			dataBytes = (size == 0 || size > avail) ? avail : size;
			if (haveFmt)
				break;
		}
		if (size > avail)
			break;
		pos += size + (size & 1);
	}

	if (!haveFmt || !data) {
		out.error = "missing fmt or data chunk";
		return false;
	}
	Format f;
	f.channels = channels;
	f.bytes = (channels > 0 && blockAlign >= channels) ? blockAlign / channels : (bits + 7) / 8;
	if (fmtTag == 3) {
		f.isFloat = true;
	}
	else if (fmtTag == 1) {
		f.unsigned8 = (f.bytes == 1);
	}
	else {
		out.error = "unsupported WAV encoding (compressed?)";
		return false;
	}
	if (f.isFloat ? (f.bytes != 4 && f.bytes != 8) : (f.bytes < 1 || f.bytes > 4)) {
		out.error = "unsupported bit depth";
		return false;
	}
	out.sampleRate = sampleRate > 0 ? (float) sampleRate : 44100.f;
	return decode(data, dataBytes, f, out);
}

static bool parseAiff(const std::vector<uint8_t>& d, bool aifc, Decoded& out) {
	size_t pos = 12;
	int channels = 0;
	int bits = 0;
	uint32_t numFrames = 0;
	double sampleRate = 0.0;
	bool haveComm = false;
	Format f;
	f.bigEndian = true;
	const uint8_t* data = NULL;
	size_t dataBytes = 0;

	while (pos + 8 <= d.size()) {
		const uint8_t* id = &d[pos];
		uint32_t size = be32(&d[pos + 4]);
		pos += 8;
		size_t avail = d.size() - pos;
		const uint8_t* p = &d[pos];
		if (std::memcmp(id, "COMM", 4) == 0 && avail >= 18) {
			channels = be16(p);
			numFrames = be32(p + 2);
			bits = be16(p + 6);
			sampleRate = ext80(p + 8);
			if (aifc && avail >= 22) {
				const uint8_t* c = p + 18;
				if (std::memcmp(c, "NONE", 4) == 0 || std::memcmp(c, "twos", 4) == 0) {
				}
				else if (std::memcmp(c, "sowt", 4) == 0) {
					f.bigEndian = false;
				}
				else if (std::memcmp(c, "fl32", 4) == 0 || std::memcmp(c, "FL32", 4) == 0) {
					f.isFloat = true;
					bits = 32;
				}
				else if (std::memcmp(c, "fl64", 4) == 0 || std::memcmp(c, "FL64", 4) == 0) {
					f.isFloat = true;
					bits = 64;
				}
				else {
					out.error = "unsupported AIFF-C compression";
					return false;
				}
			}
			haveComm = true;
		}
		else if (std::memcmp(id, "SSND", 4) == 0 && avail >= 8) {
			uint32_t offset = be32(p);
			size_t start = 8 + (size_t) offset;
			size_t len = std::min((size_t) size, avail);
			if (start < len) {
				data = p + start;
				dataBytes = len - start;
			}
		}
		if (size > avail)
			break;
		pos += size + (size & 1);
	}

	if (!haveComm || !data) {
		out.error = "missing COMM or SSND chunk";
		return false;
	}
	f.channels = channels;
	f.bytes = (bits + 7) / 8;
	if (f.isFloat ? (f.bytes != 4 && f.bytes != 8) : (f.bytes < 1 || f.bytes > 4)) {
		out.error = "unsupported bit depth";
		return false;
	}
	if (numFrames > 0 && channels > 0)
		dataBytes = std::min(dataBytes, (size_t) numFrames * channels * f.bytes);
	out.sampleRate = sampleRate > 0.0 ? (float) sampleRate : 44100.f;
	return decode(data, dataBytes, f, out);
}

bool load(const std::string& path, Decoded& out) {
	out = Decoded();
	FILE* fp = openFile(path, "rb");
	if (!fp) {
		out.error = "cannot open file";
		return false;
	}
	std::fseek(fp, 0, SEEK_END);
	long size = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if (size < 12 || (size_t) size > MAX_FILE_BYTES) {
		std::fclose(fp);
		out.error = "file too small or too large";
		return false;
	}
	std::vector<uint8_t> d((size_t) size);
	size_t got = std::fread(d.data(), 1, d.size(), fp);
	std::fclose(fp);
	d.resize(got);
	if (got < 12) {
		out.error = "read error";
		return false;
	}

	if (std::memcmp(&d[0], "RIFF", 4) == 0 && std::memcmp(&d[8], "WAVE", 4) == 0)
		return parseWav(d, out);
	if (std::memcmp(&d[0], "FORM", 4) == 0 && std::memcmp(&d[8], "AIFF", 4) == 0)
		return parseAiff(d, false, out);
	if (std::memcmp(&d[0], "FORM", 4) == 0 && std::memcmp(&d[8], "AIFC", 4) == 0)
		return parseAiff(d, true, out);
	out.error = "not a WAV/AIFF file";
	return false;
}

static void put16(std::vector<uint8_t>& b, uint16_t v) {
	b.push_back(v & 0xFF);
	b.push_back((v >> 8) & 0xFF);
}
static void put32(std::vector<uint8_t>& b, uint32_t v) {
	for (int i = 0; i < 4; i++)
		b.push_back((v >> (8 * i)) & 0xFF);
}
static void putTag(std::vector<uint8_t>& b, const char* tag) {
	b.insert(b.end(), tag, tag + 4);
}

bool writeWav(const std::string& path, const float* left, const float* right, size_t frames, float sampleRate) {
	int channels = right ? 2 : 1;
	uint32_t dataBytes = (uint32_t) (frames * channels * 4);
	std::vector<uint8_t> h;
	putTag(h, "RIFF");
	put32(h, 36 + dataBytes);
	putTag(h, "WAVE");
	putTag(h, "fmt ");
	put32(h, 16);
	put16(h, 3);
	put16(h, (uint16_t) channels);
	put32(h, (uint32_t) sampleRate);
	put32(h, (uint32_t) sampleRate * channels * 4);
	put16(h, (uint16_t) (channels * 4));
	put16(h, 32);
	putTag(h, "data");
	put32(h, dataBytes);

	FILE* fp = openFile(path, "wb");
	if (!fp)
		return false;
	bool ok = std::fwrite(h.data(), 1, h.size(), fp) == h.size();
	std::vector<uint8_t> buf;
	buf.reserve(4096 * channels * 4);
	for (size_t i = 0; i < frames && ok; i++) {
		for (int c = 0; c < channels; c++) {
			float v = (c == 0) ? left[i] : right[i];
			uint32_t u;
			std::memcpy(&u, &v, 4);
			put32(buf, u);
		}
		if (buf.size() >= 4096 * 4) {
			ok = std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
			buf.clear();
		}
	}
	if (ok && !buf.empty())
		ok = std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
	std::fclose(fp);
	return ok;
}

} // namespace audiofile
