#include "VideoComposer.h"

#include <Windows.h>
#include <combaseapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

namespace
{
	using Microsoft::WRL::ComPtr;

	struct RuntimeScope
	{
		~RuntimeScope()
		{
			if (mediaFoundation)
				MFShutdown();
			if (com)
				CoUninitialize();
		}

		bool com{ false };
		bool mediaFoundation{ false };
	};

	struct FrameMetrics
	{
		int top{};
		int bottom{};
		int leftGreen{};
		int rightGreen{};
	};

	std::optional<FrameMetrics> MeasureFrame(
		IMFSample* a_sample,
		UINT32 a_width,
		UINT32 a_height,
		LONG a_stride)
	{
		ComPtr<IMFMediaBuffer> buffer;
		BYTE* bytes = nullptr;
		DWORD length = 0;
		if (FAILED(a_sample->ConvertToContiguousBuffer(buffer.GetAddressOf())) ||
			FAILED(buffer->Lock(&bytes, nullptr, &length))) {
			return std::nullopt;
		}
		const auto absoluteStride = static_cast<std::size_t>(a_stride < 0 ? -a_stride : a_stride);
		if (length < absoluteStride * a_height) {
			buffer->Unlock();
			return std::nullopt;
		}
		auto row = [&](std::size_t a_y) {
			return a_stride > 0 ? bytes + a_y * absoluteStride : bytes + (a_height - 1 - a_y) * absoluteStride;
		};
		auto average = [&](std::size_t a_beginY, std::size_t a_endY) {
			std::uint64_t total = 0;
			std::uint64_t count = 0;
			for (auto y = a_beginY; y < a_endY; ++y) {
				const auto* pixels = row(y);
				for (std::size_t x = 0; x < a_width; ++x) {
					total += pixels[x * 4] + pixels[x * 4 + 1] + pixels[x * 4 + 2];
					count += 3;
				}
			}
			return count == 0 ? 0 : static_cast<int>(total / count);
		};
		const auto band = std::max<std::size_t>(1, a_height / 4);
		auto averageGreen = [&](std::size_t a_beginX, std::size_t a_endX) {
			std::uint64_t total = 0;
			std::uint64_t count = 0;
			for (auto y = band; y < a_height - band; ++y) {
				const auto* pixels = row(y);
				for (auto x = a_beginX; x < a_endX; ++x) {
					total += pixels[x * 4 + 1];
					++count;
				}
			}
			return count == 0 ? 0 : static_cast<int>(total / count);
		};

		FrameMetrics result;
		result.top = average(0, band);
		result.bottom = average(a_height - band, a_height);
		result.leftGreen = averageGreen(0, a_width / 2);
		result.rightGreen = averageGreen(a_width / 2, a_width);
		buffer->Unlock();
		return result;
	}

	int WaitForComposition(bool a_expectFailure)
	{
		auto& composer = CSXCaptureCompanion::VideoComposer::GetSingleton();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (std::chrono::steady_clock::now() < deadline) {
			const auto state = composer.GetState();
			if (state == CSXCaptureCompanion::ComposeState::kComplete) {
				if (a_expectFailure) {
					std::cerr << "Composer unexpectedly accepted an invalid fixture.\n";
					return 4;
				}
				std::cout << composer.GetStatusText() << '\n';
				return 0;
			}
			if (state == CSXCaptureCompanion::ComposeState::kFailed) {
				if (a_expectFailure) {
					std::cout << composer.GetStatusText() << '\n';
					return 0;
				}
				std::cerr << composer.GetStatusText() << '\n';
				return 4;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		std::cerr << "Composer smoke test timed out.\n";
		return 5;
	}

	int RunComposition(const std::filesystem::path& a_sequence, bool a_expectFailure)
	{
		auto& composer = CSXCaptureCompanion::VideoComposer::GetSingleton();
		if (!composer.Queue(a_sequence)) {
			std::cerr << "Composer rejected the smoke-test sequence before starting its worker.\n";
			return 3;
		}
		return WaitForComposition(a_expectFailure);
	}

	int RaceComposition(const std::filesystem::path& a_sequence)
	{
		auto& composer = CSXCaptureCompanion::VideoComposer::GetSingleton();
		constexpr std::size_t contenderCount = 8;
		std::barrier gate(static_cast<std::ptrdiff_t>(contenderCount + 1));
		std::atomic_size_t accepted{ 0 };
		std::vector<std::jthread> contenders;
		contenders.reserve(contenderCount);
		for (std::size_t index = 0; index < contenderCount; ++index) {
			contenders.emplace_back([&] {
				gate.arrive_and_wait();
				if (composer.Queue(a_sequence))
					accepted.fetch_add(1, std::memory_order_relaxed);
			});
		}
		gate.arrive_and_wait();
		for (auto& contender : contenders)
			contender.join();
		if (accepted.load(std::memory_order_relaxed) != 1) {
			std::cerr << "Concurrent admission accepted " << accepted << " workers.\n";
			return 6;
		}
		return WaitForComposition(false);
	}

	int VerifyOrientation(const std::filesystem::path& a_video)
	{
		RuntimeScope runtime;
		if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
			std::cerr << "CoInitializeEx failed during orientation verification.\n";
			return 7;
		}
		runtime.com = true;
		if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
			std::cerr << "MFStartup failed during orientation verification.\n";
			return 7;
		}
		runtime.mediaFoundation = true;

		ComPtr<IMFAttributes> attributes;
		if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1)) ||
			FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))) {
			std::cerr << "Could not configure the orientation decoder.\n";
			return 7;
		}
		ComPtr<IMFSourceReader> reader;
		if (FAILED(MFCreateSourceReaderFromURL(a_video.c_str(), attributes.Get(), reader.GetAddressOf()))) {
			std::cerr << "Could not open the composed MP4 for orientation verification.\n";
			return 7;
		}
		ComPtr<IMFMediaType> requestedType;
		if (FAILED(MFCreateMediaType(requestedType.GetAddressOf())) ||
			FAILED(requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
			FAILED(requestedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
			FAILED(reader->SetCurrentMediaType(
				static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, requestedType.Get()))) {
			std::cerr << "Could not request decoded RGB video.\n";
			return 7;
		}

		ComPtr<IMFSample> sample;
		DWORD flags = 0;
		LONGLONG sampleTime = 0;
		for (std::uint32_t attempt = 0; attempt < 32 && !sample; ++attempt) {
			if (FAILED(reader->ReadSample(
					static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
					0,
					nullptr,
					&flags,
					&sampleTime,
					sample.ReleaseAndGetAddressOf()))) {
				std::cerr << "Could not decode the first MP4 frame.\n";
				return 7;
			}
			if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
				break;
		}
		if (!sample) {
			std::cerr << "The composed MP4 contained no decoded frame.\n";
			return 7;
		}

		ComPtr<IMFMediaType> decodedType;
		UINT32 width = 0;
		UINT32 height = 0;
		if (FAILED(reader->GetCurrentMediaType(
				static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), decodedType.GetAddressOf())) ||
			FAILED(MFGetAttributeSize(decodedType.Get(), MF_MT_FRAME_SIZE, &width, &height)) || width == 0 || height < 4) {
			std::cerr << "The decoded MP4 dimensions were invalid.\n";
			return 7;
		}
		const auto stride = static_cast<LONG>(MFGetAttributeUINT32(decodedType.Get(), MF_MT_DEFAULT_STRIDE, width * 4));
		if (stride == 0) {
			std::cerr << "The decoded MP4 stride was invalid.\n";
			return 7;
		}

		if (width != 128 || height != 64) {
			std::cerr << "Decoded SBS dimensions were " << width << "x" << height << ", expected 128x64.\n";
			return 8;
		}
		const auto firstMetrics = MeasureFrame(sample.Get(), width, height, stride);
		if (!firstMetrics) {
			std::cerr << "Could not read the decoded MP4 frame.\n";
			return 7;
		}
		if (firstMetrics->top <= firstMetrics->bottom + 64) {
			std::cerr << "Decoded orientation check failed: top=" << firstMetrics->top <<
				", bottom=" << firstMetrics->bottom << ".\n";
			return 8;
		}
		if (firstMetrics->rightGreen <= firstMetrics->leftGreen + 64) {
			std::cerr << "Decoded SBS eye placement failed: left green=" << firstMetrics->leftGreen <<
				", right green=" << firstMetrics->rightGreen << ".\n";
			return 8;
		}

		std::vector<LONGLONG> sampleTimes{ sampleTime };
		std::vector<LONGLONG> sampleDurations;
		std::vector<FrameMetrics> metrics{ *firstMetrics };
		LONGLONG duration = 0;
		if (FAILED(sample->GetSampleDuration(&duration))) {
			std::cerr << "The first decoded sample had no duration.\n";
			return 9;
		}
		sampleDurations.push_back(duration);
		while (true) {
			ComPtr<IMFSample> nextSample;
			flags = 0;
			sampleTime = 0;
			if (FAILED(reader->ReadSample(
					static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
					0,
					nullptr,
					&flags,
					&sampleTime,
					nextSample.GetAddressOf()))) {
				std::cerr << "Could not decode the complete MP4 timeline.\n";
				return 9;
			}
			if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
				break;
			if (!nextSample)
				continue;
			if (FAILED(nextSample->GetSampleDuration(&duration))) {
				std::cerr << "A decoded sample had no duration.\n";
				return 9;
			}
			const auto nextMetrics = MeasureFrame(nextSample.Get(), width, height, stride);
			if (!nextMetrics) {
				std::cerr << "Could not read a decoded hold frame.\n";
				return 9;
			}
			sampleTimes.push_back(sampleTime);
			sampleDurations.push_back(duration);
			metrics.push_back(*nextMetrics);
		}
		const std::vector<LONGLONG> expectedTimes{
			0, 166'667, 333'333, 500'000, 666'667, 833'333, 1'000'000 };
		const std::vector<LONGLONG> expectedDurations(expectedTimes.size(), 166'666);
		if (sampleTimes.size() != expectedTimes.size()) {
			std::cerr << "Decoded timeline contained " << sampleTimes.size() <<
				" samples, expected " << expectedTimes.size() << ".\n";
			return 9;
		}
		for (std::size_t index = 0; index < sampleTimes.size(); ++index) {
			if (std::llabs(sampleTimes[index] - expectedTimes[index]) > 1 ||
				std::llabs(sampleDurations[index] - expectedDurations[index]) > 1) {
				std::cerr << "Decoded sample timing did not preserve manifest timestamps at sample " << index <<
					": time=" << sampleTimes[index] << ", duration=" << sampleDurations[index] << ".\n";
				return 9;
			}
		}
		auto held = [&](std::size_t a_left, std::size_t a_right) {
			return std::abs(metrics[a_left].leftGreen - metrics[a_right].leftGreen) <= 12 &&
			       std::abs(metrics[a_left].rightGreen - metrics[a_right].rightGreen) <= 12;
		};
		if (!held(1, 2) || !held(4, 5) || !held(5, 6)) {
			std::cerr << "Decoded samples did not preserve interior and trailing held-frame content.\n";
			return 9;
		}
		std::cout << "Decoded SBS verified: top=" << firstMetrics->top <<
			", bottom=" << firstMetrics->bottom << ", left green=" << firstMetrics->leftGreen <<
			", right green=" << firstMetrics->rightGreen << ", samples=" << sampleTimes.size() << ".\n";
		return 0;
	}

	int VerifySampleCount(const std::filesystem::path& a_video, std::size_t a_expectedCount)
	{
		RuntimeScope runtime;
		if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
			return 10;
		runtime.com = true;
		if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
			return 10;
		runtime.mediaFoundation = true;

		ComPtr<IMFAttributes> attributes;
		ComPtr<IMFSourceReader> reader;
		ComPtr<IMFMediaType> requestedType;
		if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1)) ||
			FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
			FAILED(MFCreateSourceReaderFromURL(a_video.c_str(), attributes.Get(), reader.GetAddressOf())) ||
			FAILED(MFCreateMediaType(requestedType.GetAddressOf())) ||
			FAILED(requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
			FAILED(requestedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
			FAILED(reader->SetCurrentMediaType(
				static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, requestedType.Get()))) {
			std::cerr << "Could not configure the sample-count decoder.\n";
			return 10;
		}

		std::size_t count = 0;
		bool reachedEnd = false;
		std::optional<LONGLONG> previousTime;
		for (std::size_t attempt = 0; attempt < a_expectedCount + 64; ++attempt) {
			ComPtr<IMFSample> sample;
			DWORD flags = 0;
			LONGLONG sampleTime = 0;
			if (FAILED(reader->ReadSample(
					static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
					0,
					nullptr,
					&flags,
					&sampleTime,
					sample.GetAddressOf()))) {
				std::cerr << "Could not decode the complete sample-count timeline.\n";
				return 10;
			}
			if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
				reachedEnd = true;
				break;
			}
			if (!sample)
				continue;
			LONGLONG duration = 0;
			if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0 ||
				(previousTime && sampleTime <= *previousTime)) {
				std::cerr << "Decoded sample timing was not strictly increasing.\n";
				return 10;
			}
			previousTime = sampleTime;
			++count;
			if (count > a_expectedCount)
				break;
		}
		if (!reachedEnd) {
			std::cerr << "Decoder did not report end-of-stream after the expected samples.\n";
			return 10;
		}
		if (count != a_expectedCount) {
			std::cerr << "Decoded " << count << " samples, expected " << a_expectedCount << ".\n";
			return 10;
		}
		std::cout << "Decoded sample count verified: " << count << ".\n";
		return 0;
	}
}

int wmain(int a_argumentCount, wchar_t** a_arguments)
{
	if (a_argumentCount == 3 && std::wstring_view(a_arguments[1]) == L"--expect-failure")
		return RunComposition(a_arguments[2], true);
	if (a_argumentCount == 3 && std::wstring_view(a_arguments[1]) == L"--race")
		return RaceComposition(a_arguments[2]);
	if (a_argumentCount == 3 && std::wstring_view(a_arguments[1]) == L"--verify-orientation")
		return VerifyOrientation(a_arguments[2]);
	if (a_argumentCount == 4 && std::wstring_view(a_arguments[1]) == L"--verify-sample-count") {
		wchar_t* end = nullptr;
		const auto expected = std::wcstoull(a_arguments[2], &end, 10);
		if (!end || *end != L'\0' || expected == 0)
			return 2;
		return VerifySampleCount(a_arguments[3], static_cast<std::size_t>(expected));
	}
	if (a_argumentCount == 2)
		return RunComposition(a_arguments[1], false);

	std::cerr << "Usage: CSXCaptureComposerSmoke [--expect-failure|--race|--verify-orientation] <path>\n"
		         "       CSXCaptureComposerSmoke --verify-sample-count <count> <path>\n";
	return 2;
}
