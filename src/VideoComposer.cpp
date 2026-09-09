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
#include <atomic>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace CSXCaptureCompanion
{
	namespace
	{
		std::atomic<NotificationCallback> g_notificationCallback{ nullptr };
	}

	void SetNotificationCallback(NotificationCallback a_callback) noexcept
	{
		g_notificationCallback.store(a_callback, std::memory_order_release);
	}

	void ShowNotification(std::string a_message)
	{
		if (const auto callback = g_notificationCallback.load(std::memory_order_acquire)) {
			callback(std::move(a_message));
		}
	}

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
			std::string view;
			std::string suffix;
			std::vector<Frame> frames;
			std::vector<std::uint64_t> scheduledTimestampsUs;
		};

		struct TimelinePlan
		{
			std::uint32_t frameRate{};
			std::vector<std::uint64_t> frameTicks;
			std::uint64_t finalTick{};
		};

		struct EncodingPlan
		{
			const StreamPlan* primary{};
			const StreamPlan* secondary{};
			std::filesystem::path output;
			std::filesystem::path temporary;
			TimelinePlan timeline;
		};

		constexpr std::uintmax_t kMaximumManifestBytes = 16 * 1024 * 1024;
		constexpr std::size_t kMaximumStreams = 2;
		constexpr std::size_t kMaximumSourceFrames = 60'000;
		std::atomic_uint64_t g_temporarySequence{ 1 };

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

		std::filesystem::path ManifestAssetPath(
			const std::filesystem::path& a_sequenceDirectory,
			const json& a_artifact)
		{
			if (!a_artifact.is_object() || !a_artifact.contains("path") || !a_artifact["path"].is_string()) {
				throw std::runtime_error("A completed frame is missing its artifact path.");
			}
			if (!a_artifact.contains("committed") || !a_artifact["committed"].is_boolean() ||
				!a_artifact["committed"].get<bool>()) {
				throw std::runtime_error("A completed frame references an uncommitted artifact.");
			}
			const auto wide = Utf8ToWide(a_artifact["path"].get<std::string>());
			const std::filesystem::path path(wide);
			return path.is_absolute() ? path : a_sequenceDirectory / path;
		}

		std::uint64_t ReadTimestamp(const json& a_object, std::string_view a_name)
		{
			if (!a_object.is_object() || !a_object.contains(a_name) || !a_object[a_name].is_number_unsigned())
				throw std::runtime_error("A sequence frame has an invalid timestamp.");
			return a_object[a_name].get<std::uint64_t>();
		}

		bool IsSafeSuffix(std::string_view a_suffix)
		{
			if (a_suffix.empty() || a_suffix.size() > 64)
				return false;
			return std::ranges::all_of(a_suffix, [](unsigned char a_character) {
				return (a_character >= 'a' && a_character <= 'z') ||
				       (a_character >= 'A' && a_character <= 'Z') ||
				       (a_character >= '0' && a_character <= '9') ||
				       a_character == '-' || a_character == '_';
			});
		}

		std::uint32_t EstimateFrameRate(const std::vector<std::uint64_t>& a_timestamps)
		{
			std::vector<std::uint64_t> intervals;
			for (std::size_t index = 1; index < a_timestamps.size(); ++index) {
				intervals.push_back(a_timestamps[index] - a_timestamps[index - 1]);
			}
			if (intervals.empty())
				return 60;
			const auto middle = intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2);
			std::nth_element(intervals.begin(), middle, intervals.end());
			const auto interval = std::max<std::uint64_t>(*middle, 1);
			const auto roundedRate = (1'000'000 + interval / 2) / interval;
			return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(roundedRate, 1, 120));
		}

		std::uint64_t TimestampToTick(std::uint64_t a_normalizedUs, std::uint32_t a_frameRate)
		{
			const auto wholeSeconds = a_normalizedUs / 1'000'000;
			const auto remainingUs = a_normalizedUs % 1'000'000;
			return wholeSeconds * a_frameRate + (remainingUs * a_frameRate + 500'000) / 1'000'000;
		}

		TimelinePlan BuildTimeline(const StreamPlan& a_plan)
		{
			constexpr auto maximumMicroseconds =
				static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()) / 10;
			if (a_plan.scheduledTimestampsUs.empty())
				throw std::runtime_error("The sequence contains no scheduled frame timestamps.");
			for (std::size_t index = 1; index < a_plan.scheduledTimestampsUs.size(); ++index) {
				const auto previous = a_plan.scheduledTimestampsUs[index - 1];
				const auto timestamp = a_plan.scheduledTimestampsUs[index];
				if (timestamp <= previous)
					throw std::runtime_error("Scheduled frame timestamps must be strictly increasing.");
				if (timestamp - previous > maximumMicroseconds)
					throw std::runtime_error("A scheduled frame duration is outside the supported range.");
			}

			const auto firstTimestamp = a_plan.frames.front().timestampUs;
			for (std::size_t index = 0; index < a_plan.frames.size(); ++index) {
				const auto timestamp = a_plan.frames[index].timestampUs;
				if (timestamp < firstTimestamp || timestamp - firstTimestamp > maximumMicroseconds)
					throw std::runtime_error("A frame timestamp is outside the supported Media Foundation range.");
				if (index > 0) {
					const auto previous = a_plan.frames[index - 1].timestampUs;
					if (timestamp <= previous)
						throw std::runtime_error("Frame timestamps must be strictly increasing.");
					if (timestamp - previous > maximumMicroseconds)
						throw std::runtime_error("A frame duration is outside the supported Media Foundation range.");
				}
				if (!std::binary_search(
						a_plan.scheduledTimestampsUs.begin(), a_plan.scheduledTimestampsUs.end(), timestamp)) {
					throw std::runtime_error("A written frame timestamp is absent from the scheduled timeline.");
				}
			}

			auto firstRelevant = std::lower_bound(
				a_plan.scheduledTimestampsUs.begin(), a_plan.scheduledTimestampsUs.end(), firstTimestamp);
			auto frameRate = EstimateFrameRate(a_plan.scheduledTimestampsUs);
			for (; frameRate <= 120; ++frameRate) {
				bool distinct = true;
				std::uint64_t previousTick = 0;
				for (auto timestamp = firstRelevant; timestamp != a_plan.scheduledTimestampsUs.end(); ++timestamp) {
					const auto tick = TimestampToTick(*timestamp - firstTimestamp, frameRate);
					if (timestamp != firstRelevant && tick <= previousTick) {
						distinct = false;
						break;
					}
					previousTick = tick;
				}
				if (distinct)
					break;
			}
			if (frameRate > 120)
				throw std::runtime_error("Frame timestamps are too close for the supported video cadence.");

			TimelinePlan result;
			result.frameRate = frameRate;
			result.finalTick = TimestampToTick(
				a_plan.scheduledTimestampsUs.back() - firstTimestamp, frameRate);
			if (result.finalTick >= kMaximumSourceFrames)
				throw std::runtime_error("The manifest timeline would produce too many video samples.");
			result.frameTicks.reserve(a_plan.frames.size());
			for (const auto& frame : a_plan.frames) {
				result.frameTicks.push_back(TimestampToTick(frame.timestampUs - firstTimestamp, frameRate));
			}
			return result;
		}

		std::wstring ComparablePath(const std::filesystem::path& a_path)
		{
			std::error_code error;
			auto normalized = std::filesystem::weakly_canonical(a_path, error);
			if (error) {
				error.clear();
				normalized = std::filesystem::absolute(a_path, error).lexically_normal();
				if (error)
					throw std::runtime_error("Could not normalize a capture or output path.");
			}
			auto value = normalized.native();
			std::ranges::transform(value, value.begin(), [](wchar_t a_character) {
				return static_cast<wchar_t>(std::towlower(a_character));
			});
			return value;
		}

		bool PathsAlias(const std::filesystem::path& a_left, const std::filesystem::path& a_right)
		{
			if (ComparablePath(a_left) == ComparablePath(a_right))
				return true;
			std::error_code error;
			const auto equivalent = std::filesystem::equivalent(a_left, a_right, error);
			return !error && equivalent;
		}

		std::vector<StreamPlan> ReadManifest(const std::filesystem::path& a_manifestOrDirectory)
		{
			const auto manifestPath = std::filesystem::is_directory(a_manifestOrDirectory) ?
			                          a_manifestOrDirectory / "sequence.json" :
			                          a_manifestOrDirectory;
			const auto sequenceDirectory = manifestPath.parent_path();
			std::error_code manifestError;
			const auto manifestBytes = std::filesystem::file_size(manifestPath, manifestError);
			if (manifestError || manifestBytes > kMaximumManifestBytes)
				throw std::runtime_error("The completed sequence manifest is unavailable or too large.");
			std::ifstream stream(manifestPath, std::ios::binary);
			if (!stream) {
				throw std::runtime_error("The completed sequence.json could not be opened.");
			}
			json manifest;
			stream >> manifest;
			std::vector<StreamPlan> plans;
			if (manifest.value("schema", "") == "csx.frame-sequence/1") {
				if (manifest.value("state", "") != "complete")
					throw std::runtime_error("The legacy manifest is not complete.");
				const auto eye = manifest.value("eye", "Left");
				if (eye == "Both") {
					plans.push_back({ "left_eye", "left", {}, {} });
					plans.push_back({ "right_eye", "right", {}, {} });
				} else if (eye == "Right") {
					plans.push_back({ "right_eye", "right", {}, {} });
				} else {
					plans.push_back({ "left_eye", "left", {}, {} });
				}
				const auto& frames = manifest.at("frames");
				if (!frames.is_array() || frames.size() > kMaximumSourceFrames)
					throw std::runtime_error("The legacy manifest contains too many frame records.");
				for (const auto& entry : frames) {
					const auto timestamp = ReadTimestamp(entry, "timestampUs");
					for (auto& plan : plans)
						plan.scheduledTimestampsUs.push_back(timestamp);
					if (!entry.value("written", false))
						continue;
					const auto& paths = entry.at("paths");
					if (!paths.is_array() || paths.size() != plans.size())
						throw std::runtime_error("A written frame has the wrong number of eye paths.");
					for (std::size_t index = 0; index < plans.size(); ++index) {
						const auto path = sequenceDirectory / std::filesystem::path(Utf8ToWide(paths.at(index).get<std::string>()));
						if (!std::filesystem::is_regular_file(path))
							throw std::runtime_error("A lossless source frame is missing.");
						plans[index].frames.push_back({ timestamp, path });
					}
				}
			} else {
				const auto contract = manifest.value("contract", json::object());
				if (contract.value("name", "") != "csx.screenshot" || contract.value("major", 0) != 1 ||
					manifest.value("state", "") != "final")
					throw std::runtime_error("The manifest is not a final Screenshot API v1 sequence.");
				const auto outputs = manifest.at("capture").at("outputs");
				if (!outputs.is_array() || outputs.empty())
					throw std::runtime_error("The Screenshot API manifest has no outputs.");
				if (outputs.size() > kMaximumStreams)
					throw std::runtime_error("The Screenshot API manifest has too many outputs.");
				std::set<std::string, std::less<>> suffixes;
				std::set<std::string, std::less<>> views;
				for (const auto& output : outputs) {
					if (!output.is_object())
						throw std::runtime_error("A Screenshot API output entry is invalid.");
					if (!output.contains("view") || !output["view"].is_string())
						throw std::runtime_error("A Screenshot API output has no usable view.");
					const auto view = output["view"].get<std::string>();
					if ((view != "left_eye" && view != "right_eye") || !views.insert(view).second)
						throw std::runtime_error("Video composition requires unique left- or right-eye outputs.");
					std::string suffix;
					if (output.contains("nameSuffix")) {
						if (!output["nameSuffix"].is_string())
							throw std::runtime_error("A Screenshot API output suffix is invalid.");
						suffix = output["nameSuffix"].get<std::string>();
					} else if (output.contains("view") && output["view"].is_string()) {
						suffix = output["view"].get<std::string>();
					} else {
						throw std::runtime_error("A Screenshot API output has no usable name.");
					}
					if (!IsSafeSuffix(suffix))
						throw std::runtime_error("Output suffixes must be safe filename components.");
					auto comparable = suffix;
					std::ranges::transform(comparable, comparable.begin(), [](unsigned char a_character) {
						return static_cast<char>(std::tolower(a_character));
					});
					if (!suffixes.insert(comparable).second)
						throw std::runtime_error("Output suffixes must be unique.");
					plans.push_back({ view, std::move(suffix), {}, {} });
				}
				const auto& children = manifest.at("children");
				if (!children.is_array() || children.size() > kMaximumSourceFrames)
					throw std::runtime_error("The Screenshot API manifest contains too many child records.");
				for (const auto& child : children) {
					if (!child.is_object() || !child.contains("state") || !child["state"].is_string())
						throw std::runtime_error("A sequence child has an invalid state.");
					const auto state = child["state"].get<std::string>();
					const auto timestamp = ReadTimestamp(child, "scheduledTimestampUs");
					for (auto& plan : plans)
						plan.scheduledTimestampsUs.push_back(timestamp);
					if (state != "completed" && state != "completed_with_warnings")
						continue;
					const auto& artifacts = child.at("artifacts");
					if (!artifacts.is_array() || artifacts.size() != plans.size())
						throw std::runtime_error("A completed frame has the wrong number of output artifacts.");
					for (std::size_t index = 0; index < plans.size(); ++index) {
						const auto path = ManifestAssetPath(sequenceDirectory, artifacts.at(index));
						if (!std::filesystem::is_regular_file(path))
							throw std::runtime_error("A lossless source frame is missing.");
						plans[index].frames.push_back({ timestamp, path });
					}
				}
			}

			for (const auto& plan : plans) {
				if (plan.frames.empty()) {
					throw std::runtime_error("The sequence contains no written frames to compose.");
				}
			}
			return plans;
		}

		std::vector<EncodingPlan> BuildEncodingPlans(
			const std::filesystem::path& a_sequenceDirectory,
			const std::vector<StreamPlan>& a_streams)
		{
			struct OutputSource
			{
				const StreamPlan* primary{};
				const StreamPlan* secondary{};
				std::string suffix;
				TimelinePlan timeline;
			};

			std::vector<OutputSource> sources;
			if (a_streams.size() == 1) {
				sources.push_back({
					&a_streams.front(), nullptr, a_streams.front().suffix, BuildTimeline(a_streams.front()) });
			} else {
				const StreamPlan* left = nullptr;
				const StreamPlan* right = nullptr;
				for (const auto& stream : a_streams) {
					if (stream.view == "left_eye")
						left = &stream;
					else if (stream.view == "right_eye")
						right = &stream;
				}
				if (!left || !right || left->frames.size() != right->frames.size() ||
					left->scheduledTimestampsUs != right->scheduledTimestampsUs) {
					throw std::runtime_error("A stereo sequence requires synchronized left and right eyes.");
				}
				for (std::size_t index = 0; index < left->frames.size(); ++index) {
					if (left->frames[index].timestampUs != right->frames[index].timestampUs)
						throw std::runtime_error("Stereo eye frames have different timestamps.");
				}
				sources.push_back({ left, right, "sbs", BuildTimeline(*left) });
			}

			const auto outputDirectory = a_sequenceDirectory.parent_path();
			const auto sequenceName = a_sequenceDirectory.filename().wstring();
			std::vector<EncodingPlan> result;
			std::set<std::wstring> reserved;
			std::uint32_t outputOrdinal = 1;
			for (; outputOrdinal <= 10'000; ++outputOrdinal) {
				const auto available = std::ranges::all_of(sources, [&](const OutputSource& a_source) {
					const auto baseName = sequenceName + L"-" + Utf8ToWide(a_source.suffix);
					const auto ordinal = outputOrdinal == 1 ? std::wstring{} : L"-" + std::to_wstring(outputOrdinal);
					return !std::filesystem::exists(outputDirectory / (baseName + ordinal + L".mp4"));
				});
				if (available)
					break;
			}
			if (outputOrdinal > 10'000)
				throw std::runtime_error("No available video output name could be found.");

			for (const auto& source : sources) {
				const auto suffix = Utf8ToWide(source.suffix);
				const auto baseName = sequenceName + L"-" + suffix;
				const auto ordinal = outputOrdinal == 1 ? std::wstring{} : L"-" + std::to_wstring(outputOrdinal);
				auto output = outputDirectory / (baseName + ordinal + L".mp4");

				std::filesystem::path temporary;
				for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
					const auto token = g_temporarySequence.fetch_add(1, std::memory_order_relaxed);
					temporary = outputDirectory /
						(baseName + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
							std::to_wstring(token) + L".tmp.mp4");
					if (!std::filesystem::exists(temporary))
						break;
					temporary.clear();
				}
				if (temporary.empty())
					throw std::runtime_error("No available temporary video output name could be found.");

				if (!reserved.insert(ComparablePath(output)).second ||
					!reserved.insert(ComparablePath(temporary)).second) {
					throw std::runtime_error("Video output paths must be unique.");
				}
				result.push_back({
					source.primary, source.secondary, std::move(output), std::move(temporary), source.timeline });
			}

			for (const auto& encoding : result) {
				for (const auto& stream : a_streams) {
					for (const auto& frame : stream.frames) {
						if (PathsAlias(frame.path, encoding.output) || PathsAlias(frame.path, encoding.temporary))
							throw std::runtime_error("A video output path aliases a lossless source frame.");
					}
				}
			}
			return result;
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

			DecodedFrame decoded;
			Check(converter->GetSize(&decoded.width, &decoded.height), "IWICBitmapSource::GetSize");
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
			// WIC returns top-down scan lines. MF_MT_DEFAULT_STRIDE is positive
			// for top-down images, so pass those rows through unchanged.
			Check(converter->CopyPixels(
				nullptr,
				decoded.stride,
				static_cast<std::uint32_t>(bytes),
				decoded.pixels.data()),
				"IWICBitmapSource::CopyPixels");
			return decoded;
		}

		DecodedFrame DecodeOutputFrame(
			IWICImagingFactory* a_factory,
			const EncodingPlan& a_plan,
			std::size_t a_index)
		{
			auto primary = DecodeFrameAsset(a_factory, a_plan.primary->frames.at(a_index).path);
			if (!a_plan.secondary)
				return primary;

			auto secondary = DecodeFrameAsset(a_factory, a_plan.secondary->frames.at(a_index).path);
			if (primary.width != secondary.width || primary.height != secondary.height)
				throw std::runtime_error("Stereo source eyes have different dimensions.");
			if (primary.width > std::numeric_limits<std::uint32_t>::max() - secondary.width)
				throw std::runtime_error("The side-by-side video width is too large.");

			DecodedFrame combined;
			combined.width = primary.width + secondary.width;
			combined.height = primary.height;
			if (combined.width > std::numeric_limits<std::uint32_t>::max() / 4)
				throw std::runtime_error("The side-by-side video stride is too large.");
			combined.stride = combined.width * 4;
			const auto bytes = static_cast<std::uint64_t>(combined.stride) * combined.height;
			if (bytes > std::numeric_limits<std::uint32_t>::max())
				throw std::runtime_error("The side-by-side frame is too large for a Media Foundation sample.");
			combined.pixels.resize(static_cast<std::size_t>(bytes));
			for (std::uint32_t row = 0; row < combined.height; ++row) {
				auto* destination = combined.pixels.data() + static_cast<std::size_t>(row) * combined.stride;
				const auto* left = primary.pixels.data() + static_cast<std::size_t>(row) * primary.stride;
				const auto* right = secondary.pixels.data() + static_cast<std::size_t>(row) * secondary.stride;
				std::copy_n(left, primary.stride, destination);
				std::copy_n(right, secondary.stride, destination + primary.stride);
			}
			return combined;
		}

		void WriteFrameSample(
			IMFSinkWriter* a_writer,
			DWORD a_streamIndex,
			const DecodedFrame& a_frame,
			LONGLONG a_sampleTime,
			LONGLONG a_sampleDuration)
		{
			ComPtr<IMFMediaBuffer> buffer;
			Check(MFCreateMemoryBuffer(static_cast<DWORD>(a_frame.pixels.size()), buffer.GetAddressOf()),
				"MFCreateMemoryBuffer");
			BYTE* destination = nullptr;
			Check(buffer->Lock(&destination, nullptr, nullptr), "IMFMediaBuffer::Lock");
			std::copy(a_frame.pixels.begin(), a_frame.pixels.end(), destination);
			Check(buffer->Unlock(), "IMFMediaBuffer::Unlock");
			Check(buffer->SetCurrentLength(static_cast<DWORD>(a_frame.pixels.size())), "Set sample length");

			ComPtr<IMFSample> sample;
			Check(MFCreateSample(sample.GetAddressOf()), "MFCreateSample");
			Check(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer");
			Check(sample->SetSampleTime(a_sampleTime), "Set sample time");
			Check(sample->SetSampleDuration(a_sampleDuration), "Set sample duration");
			Check(a_writer->WriteSample(a_streamIndex, sample.Get()), "IMFSinkWriter::WriteSample");
		}

		void EncodeStream(IWICImagingFactory* a_factory, const EncodingPlan& a_plan)
		{
			const auto first = DecodeOutputFrame(a_factory, a_plan, 0);
			const auto& timeline = a_plan.primary->frames;
			const auto frameRate = a_plan.timeline.frameRate;
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
				a_plan.temporary.c_str(),
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

			const auto sampleDuration = static_cast<LONGLONG>(10'000'000 / frameRate);
			std::uint64_t previousTick = 0;
			DecodedFrame previousFrame = first;
			for (std::size_t index = 0; index < timeline.size(); ++index) {
				auto decoded = index == 0 ? first : DecodeOutputFrame(a_factory, a_plan, index);
				if (decoded.width != first.width || decoded.height != first.height) {
					throw std::runtime_error("Source frame dimensions changed during the sequence.");
				}
				const auto tick = a_plan.timeline.frameTicks[index];
				for (auto missingTick = previousTick + 1; index > 0 && missingTick < tick; ++missingTick) {
					WriteFrameSample(writer.Get(), streamIndex, previousFrame,
						static_cast<LONGLONG>(missingTick) * sampleDuration, sampleDuration);
				}
				WriteFrameSample(writer.Get(), streamIndex, decoded,
					static_cast<LONGLONG>(tick) * sampleDuration, sampleDuration);
				previousTick = tick;
				previousFrame = std::move(decoded);
			}
			for (auto trailingTick = previousTick + 1; trailingTick <= a_plan.timeline.finalTick; ++trailingTick) {
				WriteFrameSample(writer.Get(), streamIndex, previousFrame,
					static_cast<LONGLONG>(trailingTick) * sampleDuration, sampleDuration);
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
		std::lock_guard workerLock(workerMutex);
		if (workerActive) {
			ShowNotification("Video composer is busy");
			return false;
		}
		workerActive = true;
		try {
			SetStatus(ComposeState::kQueued, "Video composition queued.");
			ShowNotification("Video composition queued");
			worker = std::jthread([this, sequenceDirectory = a_sequenceDirectory] {
				try {
					Run(sequenceDirectory);
				} catch (const std::exception& exception) {
					SetStatus(ComposeState::kFailed, std::string("Video worker failed unexpectedly: ") + exception.what());
					SKSE::log::error("{}", GetStatusText());
				} catch (...) {
					SetStatus(ComposeState::kFailed, "Video worker failed with an unknown error.");
					SKSE::log::error("{}", GetStatusText());
				}
				std::lock_guard completionLock(workerMutex);
				workerActive = false;
			});
		} catch (const std::exception& exception) {
			workerActive = false;
			SetStatus(ComposeState::kFailed, std::string("Video worker could not start: ") + exception.what());
			ShowNotification("Video composition failed - see CSXCaptureCompanion.log");
			return false;
		} catch (...) {
			workerActive = false;
			SetStatus(ComposeState::kFailed, "Video worker could not start.");
			ShowNotification("Video composition failed - see CSXCaptureCompanion.log");
			return false;
		}
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
			if (!std::filesystem::is_directory(a_sequenceDirectory))
				a_sequenceDirectory = a_sequenceDirectory.parent_path();
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
			const auto encodings = BuildEncodingPlans(a_sequenceDirectory, plans);
			std::vector<std::filesystem::path> completedOutputs;
			for (const auto& encoding : encodings) {
				if (std::filesystem::exists(encoding.temporary))
					throw std::runtime_error("The selected temporary video path is no longer available.");
				EncodeStream(factory.Get(), encoding);
				if (!MoveFileExW(
						encoding.temporary.c_str(),
						encoding.output.c_str(),
						MOVEFILE_WRITE_THROUGH)) {
					throw std::runtime_error("Could not commit the completed MP4 output.");
				}
				completedOutputs.push_back(encoding.output);
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
			ShowNotification("Video composition complete");
		} catch (const std::exception& exception) {
			SetStatus(ComposeState::kFailed, std::string("Video composition failed: ") + exception.what());
			SKSE::log::error("{}", GetStatusText());
			ShowNotification("Video composition failed - see CSXCaptureCompanion.log");
		} catch (...) {
			SetStatus(ComposeState::kFailed, "Video composition failed with an unknown error.");
			SKSE::log::error("{}", GetStatusText());
			ShowNotification("Video composition failed - see CSXCaptureCompanion.log");
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
