#include "VideoComposer.h"

#include <Windows.h>
#include <combaseapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <nlohmann/json.hpp>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace CSXCaptureCompanion
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using json = nlohmann::json;

		struct Frame
		{
			std::uint64_t timestampUs{};
			std::filesystem::path path;
		};

		struct StreamPlan
		{
			std::string suffix;
			std::vector<Frame> frames;
		};

		class ComRuntime final
		{
		public:
			ComRuntime()
			{
				const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				if (FAILED(result)) {
					Throw(result, "CoInitializeEx");
				}
				initialized = true;
			}

			~ComRuntime()
			{
				if (initialized) {
					CoUninitialize();
				}
			}

			ComRuntime(const ComRuntime&) = delete;
			ComRuntime& operator=(const ComRuntime&) = delete;

			static void Throw(HRESULT a_result, std::string_view a_operation)
			{
				throw std::runtime_error(
					std::string(a_operation) + " failed (HRESULT " +
					std::to_string(static_cast<std::uint32_t>(a_result)) + ")");
			}

		private:
			bool initialized{ false };
		};

		class MediaFoundationRuntime final
		{
		public:
			MediaFoundationRuntime()
			{
				const auto result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
				if (FAILED(result)) {
					ComRuntime::Throw(result, "MFStartup");
				}
				started = true;
			}

			~MediaFoundationRuntime()
			{
				if (started) {
					MFShutdown();
				}
			}

			MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
			MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

		private:
			bool started{ false };
		};

		void Check(HRESULT a_result, std::string_view a_operation)
		{
			if (FAILED(a_result)) {
				ComRuntime::Throw(a_result, a_operation);
			}
		}

		std::wstring Utf8ToWide(const std::string& a_value)
		{
			if (a_value.empty()) {
				return {};
			}
			if (a_value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
				throw std::runtime_error("UTF-8 path is too long.");
			}
			const auto inputLength = static_cast<int>(a_value.size());
			const auto outputLength = MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				a_value.data(),
				inputLength,
				nullptr,
				0);
			if (outputLength <= 0) {
				throw std::runtime_error("Manifest contains an invalid UTF-8 path.");
			}
			std::wstring result(static_cast<std::size_t>(outputLength), L'\0');
			if (MultiByteToWideChar(
					CP_UTF8,
					MB_ERR_INVALID_CHARS,
					a_value.data(),
					inputLength,
					result.data(),
					outputLength) != outputLength) {
				throw std::runtime_error("Could not convert a manifest path to UTF-16.");
			}
			return result;
		}

		std::vector<StreamPlan> ReadManifest(const std::filesystem::path& a_sequenceDirectory)
		{
			const auto manifestPath = a_sequenceDirectory / "sequence.json";
			std::ifstream stream(manifestPath, std::ios::binary);
			if (!stream) {
				throw std::runtime_error("The completed sequence.json could not be opened.");
			}
			json manifest;
			stream >> manifest;
			if (manifest.value("schema", "") != "csx.frame-sequence/1" ||
				manifest.value("state", "") != "complete") {
				throw std::runtime_error("The manifest is not a completed CSX frame sequence.");
			}

			const auto eye = manifest.value("eye", "Left");
			std::vector<StreamPlan> plans;
			if (eye == "Both") {
				plans.push_back({ "-left", {} });
				plans.push_back({ "-right", {} });
			} else if (eye == "Right") {
				plans.push_back({ "-right", {} });
			} else {
				plans.push_back({ "-left", {} });
			}

			for (const auto& entry : manifest.at("frames")) {
				if (!entry.value("written", false)) {
					continue;
				}
				const auto timestamp = entry.at("timestampUs").get<std::uint64_t>();
				const auto& paths = entry.at("paths");
				if (!paths.is_array() || paths.size() != plans.size()) {
					throw std::runtime_error("A written frame has the wrong number of eye paths.");
				}
				for (std::size_t index = 0; index < plans.size(); ++index) {
					const auto relativeWide = Utf8ToWide(paths.at(index).get<std::string>());
					const auto path = a_sequenceDirectory / std::filesystem::path(relativeWide);
					if (!std::filesystem::is_regular_file(path)) {
						throw std::runtime_error("A lossless source frame is missing.");
					}
					plans[index].frames.push_back({ timestamp, path });
				}
			}

			for (const auto& plan : plans) {
				if (plan.frames.empty()) {
					throw std::runtime_error("The sequence contains no written frames to compose.");
				}
			}
			return plans;
		}

		struct DecodedFrame
		{
			std::uint32_t width{};
			std::uint32_t height{};
			std::uint32_t stride{};
			std::vector<std::uint8_t> pixels;
		};

		DecodedFrame DecodeFrameAsset(IWICImagingFactory* a_factory, const std::filesystem::path& a_path)
		{
			ComPtr<IWICBitmapDecoder> decoder;
			Check(a_factory->CreateDecoderFromFilename(
				a_path.c_str(),
				nullptr,
				GENERIC_READ,
				WICDecodeMetadataCacheOnLoad,
				decoder.GetAddressOf()),
				"IWICImagingFactory::CreateDecoderFromFilename");

			ComPtr<IWICBitmapFrameDecode> frame;
			Check(decoder->GetFrame(0, frame.GetAddressOf()), "IWICBitmapDecoder::GetFrame");
			ComPtr<IWICFormatConverter> converter;
			Check(a_factory->CreateFormatConverter(converter.GetAddressOf()), "IWICImagingFactory::CreateFormatConverter");
			Check(converter->Initialize(
				frame.Get(),
				GUID_WICPixelFormat32bppBGRA,
				WICBitmapDitherTypeNone,
				nullptr,
				0.0,
				WICBitmapPaletteTypeCustom),
				"IWICFormatConverter::Initialize");

			// Media Foundation RGB32 uses a bottom-up positive stride. Flip WIC's
			// top-down pixels here so the encoded video is upright.
			ComPtr<IWICBitmapFlipRotator> flip;
			Check(a_factory->CreateBitmapFlipRotator(flip.GetAddressOf()), "IWICImagingFactory::CreateBitmapFlipRotator");
			Check(flip->Initialize(converter.Get(), WICBitmapTransformFlipVertical), "IWICBitmapFlipRotator::Initialize");

			DecodedFrame decoded;
			Check(flip->GetSize(&decoded.width, &decoded.height), "IWICBitmapSource::GetSize");
			if (decoded.width == 0 || decoded.height == 0 ||
				decoded.width > std::numeric_limits<std::uint32_t>::max() / 4) {
				throw std::runtime_error("A source frame has invalid dimensions.");
			}
			decoded.stride = decoded.width * 4;
			const auto bytes = static_cast<std::uint64_t>(decoded.stride) * decoded.height;
			if (bytes > std::numeric_limits<std::uint32_t>::max()) {
				throw std::runtime_error("A source frame is too large for a Media Foundation sample.");
			}
			decoded.pixels.resize(static_cast<std::size_t>(bytes));
			Check(flip->CopyPixels(
				nullptr,
				decoded.stride,
				static_cast<std::uint32_t>(bytes),
				decoded.pixels.data()),
				"IWICBitmapSource::CopyPixels");
			return decoded;
		}

		std::uint32_t EstimateFrameRate(const std::vector<Frame>& a_frames)
		{
			std::vector<std::uint64_t> intervals;
			for (std::size_t index = 1; index < a_frames.size(); ++index) {
				if (a_frames[index].timestampUs > a_frames[index - 1].timestampUs) {
					intervals.push_back(a_frames[index].timestampUs - a_frames[index - 1].timestampUs);
				}
			}
			if (intervals.empty()) {
				return 60;
			}
			const auto middle = intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2);
			std::nth_element(intervals.begin(), middle, intervals.end());
			const auto interval = std::max<std::uint64_t>(*middle, 1);
			return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(1'000'000 / interval, 1, 120));
		}

		void EncodeStream(
			IWICImagingFactory* a_factory,
			const StreamPlan& a_plan,
			const std::filesystem::path& a_outputPath)
		{
			const auto first = DecodeFrameAsset(a_factory, a_plan.frames.front().path);
			const auto frameRate = EstimateFrameRate(a_plan.frames);
			const auto pixelsPerSecond =
				static_cast<std::uint64_t>(first.width) * first.height * frameRate;
			const auto bitrate64 = std::clamp<std::uint64_t>(pixelsPerSecond / 8, 4'000'000, 50'000'000);
			const auto bitrate = static_cast<std::uint32_t>(bitrate64);

			ComPtr<IMFAttributes> attributes;
			Check(MFCreateAttributes(attributes.GetAddressOf(), 2), "MFCreateAttributes");
			Check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "IMFAttributes::SetUINT32");
			Check(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "IMFAttributes::SetUINT32");

			ComPtr<IMFSinkWriter> writer;
			Check(MFCreateSinkWriterFromURL(
				a_outputPath.c_str(),
				nullptr,
				attributes.Get(),
				writer.GetAddressOf()),
				"MFCreateSinkWriterFromURL");

			ComPtr<IMFMediaType> outputType;
			Check(MFCreateMediaType(outputType.GetAddressOf()), "MFCreateMediaType(output)");
			Check(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set output major type");
			Check(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Set H.264 subtype");
			Check(outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate), "Set video bitrate");
			Check(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set interlace mode");
			Check(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, first.width, first.height), "Set output frame size");
			Check(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, frameRate, 1), "Set output frame rate");
			Check(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set pixel aspect ratio");

			DWORD streamIndex = 0;
			Check(writer->AddStream(outputType.Get(), &streamIndex), "IMFSinkWriter::AddStream");

			ComPtr<IMFMediaType> inputType;
			Check(MFCreateMediaType(inputType.GetAddressOf()), "MFCreateMediaType(input)");
			Check(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set input major type");
			Check(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Set RGB32 subtype");
			Check(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set input interlace mode");
			Check(inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, first.stride), "Set input stride");
			Check(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, first.width, first.height), "Set input frame size");
			Check(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, frameRate, 1), "Set input frame rate");
			Check(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set input pixel aspect ratio");
			Check(writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr), "IMFSinkWriter::SetInputMediaType");
			Check(writer->BeginWriting(), "IMFSinkWriter::BeginWriting");

			const auto firstTimestamp = a_plan.frames.front().timestampUs;
			std::uint64_t fallbackDurationUs = 1'000'000 / frameRate;
			for (std::size_t index = 0; index < a_plan.frames.size(); ++index) {
				auto decoded = index == 0 ? first : DecodeFrameAsset(a_factory, a_plan.frames[index].path);
				if (decoded.width != first.width || decoded.height != first.height) {
					throw std::runtime_error("Source frame dimensions changed during the sequence.");
				}

				std::uint64_t durationUs = fallbackDurationUs;
				if (index + 1 < a_plan.frames.size() &&
					a_plan.frames[index + 1].timestampUs > a_plan.frames[index].timestampUs) {
					durationUs = a_plan.frames[index + 1].timestampUs - a_plan.frames[index].timestampUs;
					fallbackDurationUs = durationUs;
				}

				ComPtr<IMFMediaBuffer> buffer;
				Check(MFCreateMemoryBuffer(
					static_cast<DWORD>(decoded.pixels.size()),
					buffer.GetAddressOf()),
					"MFCreateMemoryBuffer");
				BYTE* destination = nullptr;
				Check(buffer->Lock(&destination, nullptr, nullptr), "IMFMediaBuffer::Lock");
				std::copy(decoded.pixels.begin(), decoded.pixels.end(), destination);
				Check(buffer->Unlock(), "IMFMediaBuffer::Unlock");
				Check(buffer->SetCurrentLength(static_cast<DWORD>(decoded.pixels.size())), "Set sample length");

				ComPtr<IMFSample> sample;
				Check(MFCreateSample(sample.GetAddressOf()), "MFCreateSample");
				Check(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer");
				const auto normalizedUs = a_plan.frames[index].timestampUs >= firstTimestamp ?
					a_plan.frames[index].timestampUs - firstTimestamp : 0;
				Check(sample->SetSampleTime(static_cast<LONGLONG>(normalizedUs * 10)), "Set sample time");
				Check(sample->SetSampleDuration(static_cast<LONGLONG>(durationUs * 10)), "Set sample duration");
				Check(writer->WriteSample(streamIndex, sample.Get()), "IMFSinkWriter::WriteSample");
			}

			Check(writer->Finalize(), "IMFSinkWriter::Finalize");
		}
	}

	VideoComposer& VideoComposer::GetSingleton()
	{
		static VideoComposer singleton;
		return singleton;
	}

	bool VideoComposer::Queue(const std::filesystem::path& a_sequenceDirectory)
	{
		const auto current = state.load(std::memory_order_acquire);
		if (current == ComposeState::kQueued || current == ComposeState::kEncoding) {
			return false;
		}
		SetStatus(ComposeState::kQueued, "Video composition queued.");
		worker = std::jthread([this, sequenceDirectory = a_sequenceDirectory] {
			Run(sequenceDirectory);
		});
		return true;
	}

	ComposeState VideoComposer::GetState() const noexcept
	{
		return state.load(std::memory_order_acquire);
	}

	std::string VideoComposer::GetStatusText() const
	{
		std::lock_guard lock(statusMutex);
		return statusText;
	}

	void VideoComposer::Run(std::filesystem::path a_sequenceDirectory)
	{
		try {
			SetStatus(ComposeState::kEncoding, "Encoding the latest completed capture.");
			ComRuntime com;
			MediaFoundationRuntime mediaFoundation;
			ComPtr<IWICImagingFactory> factory;
			Check(CoCreateInstance(
				CLSID_WICImagingFactory,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(factory.GetAddressOf())),
				"CoCreateInstance(WICImagingFactory)");

			const auto plans = ReadManifest(a_sequenceDirectory);
			std::vector<std::filesystem::path> completedOutputs;
			for (const auto& plan : plans) {
				const auto baseName = a_sequenceDirectory.filename().wstring() +
					std::wstring(plan.suffix.begin(), plan.suffix.end());
				const auto output = a_sequenceDirectory.parent_path() / (baseName + L".mp4");
				const auto temporary = a_sequenceDirectory.parent_path() / (baseName + L".tmp.mp4");
				std::error_code ec;
				std::filesystem::remove(temporary, ec);
				EncodeStream(factory.Get(), plan, temporary);
				if (!MoveFileExW(
						temporary.c_str(),
						output.c_str(),
						MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
					throw std::runtime_error("Could not commit the completed MP4 output.");
				}
				completedOutputs.push_back(output);
			}

			std::string message = "Completed: ";
			for (std::size_t index = 0; index < completedOutputs.size(); ++index) {
				if (index != 0) {
					message += ", ";
				}
				message += completedOutputs[index].filename().string();
			}
			SetStatus(ComposeState::kComplete, std::move(message));
			SKSE::log::info("{}", GetStatusText());
		} catch (const std::exception& exception) {
			SetStatus(ComposeState::kFailed, std::string("Video composition failed: ") + exception.what());
			SKSE::log::error("{}", GetStatusText());
		} catch (...) {
			SetStatus(ComposeState::kFailed, "Video composition failed with an unknown error.");
			SKSE::log::error("{}", GetStatusText());
		}
	}

	void VideoComposer::SetStatus(ComposeState a_state, std::string a_text)
	{
		{
			std::lock_guard lock(statusMutex);
			statusText = std::move(a_text);
		}
		state.store(a_state, std::memory_order_release);
	}
}
