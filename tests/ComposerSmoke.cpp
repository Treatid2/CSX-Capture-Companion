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
#include <filesystem>
#include <iostream>
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

		ComPtr<IMFMediaBuffer> buffer;
		BYTE* bytes = nullptr;
		DWORD length = 0;
		if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf())) ||
			FAILED(buffer->Lock(&bytes, nullptr, &length))) {
			std::cerr << "Could not read the decoded MP4 frame.\n";
			return 7;
		}
		const auto absoluteStride = static_cast<std::size_t>(stride < 0 ? -stride : stride);
		if (length < absoluteStride * height) {
			buffer->Unlock();
			std::cerr << "The decoded MP4 frame buffer was truncated.\n";
			return 7;
		}
		auto row = [&](std::size_t a_y) {
			return stride > 0 ? bytes + a_y * absoluteStride : bytes + (height - 1 - a_y) * absoluteStride;
		};
		auto average = [&](std::size_t a_begin, std::size_t a_end) {
			std::uint64_t total = 0;
			std::uint64_t count = 0;
			for (auto y = a_begin; y < a_end; ++y) {
				const auto* pixels = row(y);
				for (std::size_t x = 0; x < width; ++x) {
					total += pixels[x * 4] + pixels[x * 4 + 1] + pixels[x * 4 + 2];
					count += 3;
				}
			}
			return count == 0 ? 0 : static_cast<int>(total / count);
		};
		const auto band = std::max<std::size_t>(1, height / 4);
		const auto top = average(0, band);
		const auto bottom = average(height - band, height);
		if (width != 128 || height != 64) {
			buffer->Unlock();
			std::cerr << "Decoded SBS dimensions were " << width << "x" << height << ", expected 128x64.\n";
			return 8;
		}
		auto averageGreen = [&](std::size_t a_beginX, std::size_t a_endX) {
			std::uint64_t total = 0;
			std::uint64_t count = 0;
			for (auto y = band; y < height - band; ++y) {
				const auto* pixels = row(y);
				for (auto x = a_beginX; x < a_endX; ++x) {
					total += pixels[x * 4 + 1];
					++count;
				}
			}
			return count == 0 ? 0 : static_cast<int>(total / count);
		};
		const auto leftGreen = averageGreen(0, width / 2);
		const auto rightGreen = averageGreen(width / 2, width);
		buffer->Unlock();
		if (top <= bottom + 64) {
			std::cerr << "Decoded orientation check failed: top=" << top << ", bottom=" << bottom << ".\n";
			return 8;
		}
		if (rightGreen <= leftGreen + 64) {
			std::cerr << "Decoded SBS eye placement failed: left green=" << leftGreen <<
				", right green=" << rightGreen << ".\n";
			return 8;
		}

		std::vector<LONGLONG> sampleTimes{ sampleTime };
		std::vector<LONGLONG> sampleDurations;
		LONGLONG duration = 0;
		if (FAILED(sample->GetSampleDuration(&duration))) {
			std::cerr << "The first decoded sample had no duration.\n";
			return 9;
		}
		sampleDurations.push_back(duration);
		while (sampleTimes.size() < 5) {
			ComPtr<IMFSample> nextSample;
			flags = 0;
			sampleTime = 0;
			if (FAILED(reader->ReadSample(
					static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
					0,
					nullptr,
					&flags,
					&sampleTime,
					nextSample.GetAddressOf())) ||
				(flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || !nextSample ||
				FAILED(nextSample->GetSampleDuration(&duration))) {
				std::cerr << "The composed MP4 did not retain the dropped-frame hold sample.\n";
				return 9;
			}
			sampleTimes.push_back(sampleTime);
			sampleDurations.push_back(duration);
		}
		const std::vector<LONGLONG> expectedTimes{ 0, 169'491, 338'983, 508'474, 677'966 };
		const std::vector<LONGLONG> expectedDurations(5, 169'491);
		for (std::size_t index = 0; index < sampleTimes.size(); ++index) {
			if (sampleTimes[index] != expectedTimes[index] || sampleDurations[index] != expectedDurations[index]) {
				std::cerr << "Decoded sample timing did not preserve manifest timestamps at sample " << index <<
					": time=" << sampleTimes[index] << ", duration=" << sampleDurations[index] << ".\n";
				return 9;
			}
		}
		std::cout << "Decoded SBS verified: top=" << top << ", bottom=" << bottom <<
			", left green=" << leftGreen << ", right green=" << rightGreen << ".\n";
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
	if (a_argumentCount == 2)
		return RunComposition(a_arguments[1], false);

	std::cerr << "Usage: CSXCaptureComposerSmoke [--expect-failure|--race|--verify-orientation] <path>\n";
	return 2;
}
