#include "CSXServiceAPI.h"
#include "CSXScreenshotAPI.h"
#include "VideoComposer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>

namespace
{
	using json = nlohmann::json;

	std::atomic<const CSX::ScreenshotAPI::Interface001*> g_screenshot{ nullptr };
	std::atomic_int g_eye{ 0 };
	std::atomic_uint64_t g_commandSequence{ 1 };
	std::mutex g_captureMutex;
	std::string g_activeSequenceId;
	std::filesystem::path g_latestManifestPath;
	std::int32_t g_lastCaptureState = 0;

	void ShowInGameNotification(std::string a_message)
	{
		if (auto* taskInterface = SKSE::GetTaskInterface()) {
			taskInterface->AddTask([message = std::move(a_message)] {
				RE::SendHUDMessage::ShowHUDMessage(message.c_str(), nullptr, true);
			});
		}
	}

	std::string NextCommandId(std::string_view a_action)
	{
		return std::format(
			"{}:{}:{}",
			a_action,
			GetTickCount64(),
			g_commandSequence.fetch_add(1, std::memory_order_relaxed));
	}

	json Dispatch(json a_request)
	{
		const auto* service = g_screenshot.load(std::memory_order_acquire);
		if (!service || !service->Dispatch)
			return { { "ok", false }, { "error", { { "code", "service_unavailable" } } } };

		a_request["contractMajor"] = CSX::ScreenshotAPI::ServiceMajor;
		a_request["clientId"] = "csx.capture.companion";
		if (!a_request.contains("commandId"))
			a_request["commandId"] = NextCommandId(a_request.value("action", std::string("request")));
		const auto text = a_request.dump();
		CSX::ScreenshotAPI::Request001 request;
		request.jsonUtf8 = text.data();
		request.jsonBytes = static_cast<std::uint32_t>(text.size());
		CSX::ScreenshotAPI::Response001 response;
		const auto status = service->Dispatch(service->context, &request, &response);
		if (status != CSX::ScreenshotAPI::Status::kSuccess || !response.jsonUtf8) {
			SKSE::log::error("CSX screenshot transport failed ({})", static_cast<std::uint32_t>(status));
			return {
				{ "ok", false },
				{ "error", { { "code", "transport_error" }, { "status", static_cast<std::uint32_t>(status) } } },
			};
		}
		try {
			return json::parse(response.jsonUtf8, response.jsonUtf8 + response.jsonBytes);
		} catch (const json::exception& error) {
			SKSE::log::error("CSX screenshot response was invalid JSON: {}", error.what());
			return { { "ok", false }, { "error", { { "code", "invalid_response" } } } };
		}
	}

	bool ConnectToCSX()
	{
		CSX::ServiceAPI::RegistryMessage001 request;
		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging || !messaging->Dispatch(
				CSX::ServiceAPI::RegistryMessageType,
				&request,
				sizeof(request),
				CSX::ServiceAPI::ProviderName) ||
			request.status != CSX::ServiceAPI::Status::kSuccess || !request.registry) {
			SKSE::log::warn("CSX service-registry discovery failed");
			return false;
		}

		CSX::ServiceAPI::ServiceQuery001 query;
		query.name = CSX::ScreenshotAPI::ServiceName;
		query.major = CSX::ScreenshotAPI::ServiceMajor;
		query.minimumMinor = CSX::ScreenshotAPI::ServiceMinor;
		query.maximumMinor = CSX::ScreenshotAPI::ServiceMinor;
		query.requiredCapabilities =
			CSX::ServiceAPI::kCapabilityInspection |
			CSX::ServiceAPI::kCapabilityRuntimeMutation |
			CSX::ServiceAPI::kCapabilityAsynchronousOperations |
			CSX::ServiceAPI::kCapabilityEventStream;
		const void* opaque = nullptr;
		CSX::ServiceAPI::ServiceDescriptor001 descriptor;
		const auto status = request.registry->QueryService(
			request.registry->context, &query, &opaque, &descriptor);
		if (status != CSX::ServiceAPI::Status::kSuccess || !opaque) {
			SKSE::log::warn("CSX Screenshot API v1 is unavailable ({})", static_cast<std::uint32_t>(status));
			g_screenshot.store(nullptr, std::memory_order_release);
			return false;
		}

		auto* service = static_cast<const CSX::ScreenshotAPI::Interface001*>(opaque);
		if (service->structSize < sizeof(CSX::ScreenshotAPI::Interface001) || !service->Dispatch) {
			SKSE::log::warn("CSX Screenshot API v1 returned an undersized interface");
			return false;
		}
		g_screenshot.store(service, std::memory_order_release);
		SKSE::log::info("Connected to CSX Screenshot API {}.{} through CSXR", descriptor.major, descriptor.minor);
		return true;
	}

	std::int32_t SanitizeEye(std::int32_t a_eye)
	{
		return a_eye == 1 || a_eye == 2 ? a_eye : 0;
	}

	json CaptureDescriptor(std::int32_t a_eye, std::string_view a_format)
	{
		json outputs = json::array();
		auto append = [&](std::string_view a_view, std::string_view a_suffix) {
			outputs.push_back({
				{ "view", a_view },
				{ "encoding", { { "format", a_format }, { "colourContract", "sdr_srgb" } } },
				{ "nameSuffix", a_suffix },
			});
		};
		if (a_eye == 2) {
			append("left_eye", "left");
			append("right_eye", "right");
		} else if (a_eye == 1) {
			append("right_eye", "right");
		} else {
			append("left_eye", "left");
		}
		return {
			{ "source", { { "kind", "hmd_submission" }, { "fallback", "reject" } } },
			{ "outputs", std::move(outputs) },
			{ "destination", { { "policy", "settings_default" }, { "overwrite", "never" } } },
			{ "clipboard", "none" },
			{ "tags", { { "producer", "CSX Capture Companion" } } },
		};
	}

	bool IsTerminal(std::string_view a_state)
	{
		return a_state == "completed" || a_state == "completed_with_warnings" ||
		       a_state == "failed" || a_state == "failed_partial" ||
		       a_state == "rejected" || a_state == "cancelled" ||
		       a_state == "cancelled_partial" || a_state == "stopped" ||
		       a_state == "dropped";
	}

	std::int32_t CaptureStateCode(std::string_view a_state)
	{
		if (a_state == "stop_requested" || a_state == "finalizing")
			return 2;
		if (a_state == "completed" || a_state == "completed_with_warnings" || a_state == "stopped")
			return 3;
		if (a_state == "failed" || a_state == "failed_partial" || a_state == "rejected" ||
			a_state == "cancelled" || a_state == "cancelled_partial" || a_state == "dropped")
			return 4;
		return a_state.empty() ? 0 : 1;
	}

	std::int32_t RefreshSequenceState()
	{
		std::string requestId;
		{
			std::lock_guard lock(g_captureMutex);
			requestId = g_activeSequenceId;
			if (requestId.empty())
				return g_lastCaptureState;
		}
		const auto response = Dispatch({ { "action", "request_get" }, { "requestId", requestId } });
		if (!response.value("ok", false) || !response.contains("result"))
			return -1;

		const auto& receipt = response["result"];
		const auto state = receipt.value("state", std::string{});
		const auto code = CaptureStateCode(state);
		std::lock_guard lock(g_captureMutex);
		g_lastCaptureState = code;
		if (IsTerminal(state)) {
			if (receipt.contains("manifest") && receipt["manifest"].is_object()) {
				const auto& finalPath = receipt["manifest"]["finalPath"];
				if (finalPath.is_string())
					g_latestManifestPath = std::filesystem::u8path(finalPath.get<std::string>());
			}
			g_activeSequenceId.clear();
		}
		return code;
	}

	bool IsAvailable(RE::StaticFunctionTag*)
	{
		return g_screenshot.load(std::memory_order_acquire) != nullptr;
	}

	void SetEye(RE::StaticFunctionTag*, std::int32_t a_eye)
	{
		g_eye.store(SanitizeEye(a_eye), std::memory_order_release);
	}

	bool TakeScreenshot(RE::StaticFunctionTag*)
	{
		const auto response = Dispatch({
			{ "action", "capture" }, { "useSettings", false },
			{ "capture", CaptureDescriptor(g_eye.load(std::memory_order_acquire), "png") },
		});
		return response.value("ok", false);
	}

	bool ToggleFrameSequence(RE::StaticFunctionTag*)
	{
		if (!g_screenshot.load(std::memory_order_acquire))
			return false;
		(void)RefreshSequenceState();
		std::string activeId;
		{
			std::lock_guard lock(g_captureMutex);
			activeId = g_activeSequenceId;
		}
		if (!activeId.empty()) {
			const auto response = Dispatch({ { "action", "sequence_stop" }, { "requestId", activeId } });
			if (response.value("ok", false)) {
				std::lock_guard lock(g_captureMutex);
				g_lastCaptureState = 2;
				return true;
			}
			return false;
		}

		const auto response = Dispatch({
			{ "action", "sequence_start" },
			{ "sequence", {
				{ "frameCount", 300 }, { "useSettings", false },
				{ "schedule", { { "basis", "game_frames" }, { "intervalFrames", 12 } } },
				{ "backpressure", { { "policy", "skip" }, { "maximumConsecutiveSkips", 5 } } },
				{ "failurePolicy", "continue" },
				{ "capture", CaptureDescriptor(g_eye.load(std::memory_order_acquire), "bmp") },
				{ "packaging", { { "frameManifest", true }, { "previewVideo", { { "requested", false } } } } },
			} },
		});
		if (!response.value("ok", false) || !response.contains("result"))
			return false;
		const auto requestId = response["result"].value("requestId", std::string{});
		if (requestId.empty())
			return false;
		std::lock_guard lock(g_captureMutex);
		g_activeSequenceId = requestId;
		g_lastCaptureState = 1;
		return true;
	}

	std::int32_t GetCaptureState(RE::StaticFunctionTag*)
	{
		if (!g_screenshot.load(std::memory_order_acquire))
			return -1;
		return RefreshSequenceState();
	}

	bool ComposeLatestVideo(RE::StaticFunctionTag*)
	{
		(void)RefreshSequenceState();
		std::filesystem::path manifest;
		{
			std::lock_guard lock(g_captureMutex);
			manifest = g_latestManifestPath;
		}
		if (manifest.empty()) {
			SKSE::log::warn("Compose requested without a completed Screenshot API sequence receipt");
			CSXCaptureCompanion::ShowNotification("No completed capture is ready");
			return false;
		}
		return CSXCaptureCompanion::VideoComposer::GetSingleton().Queue(manifest);
	}

	std::int32_t GetComposeState(RE::StaticFunctionTag*)
	{
		return static_cast<std::int32_t>(CSXCaptureCompanion::VideoComposer::GetSingleton().GetState());
	}

	std::string GetComposeStatus(RE::StaticFunctionTag*)
	{
		return CSXCaptureCompanion::VideoComposer::GetSingleton().GetStatusText();
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("IsAvailable", "CSXCaptureNative", IsAvailable);
		a_vm->RegisterFunction("SetEye", "CSXCaptureNative", SetEye);
		a_vm->RegisterFunction("TakeScreenshot", "CSXCaptureNative", TakeScreenshot);
		a_vm->RegisterFunction("ToggleFrameSequence", "CSXCaptureNative", ToggleFrameSequence);
		a_vm->RegisterFunction("GetCaptureState", "CSXCaptureNative", GetCaptureState);
		a_vm->RegisterFunction("ComposeLatestVideo", "CSXCaptureNative", ComposeLatestVideo);
		a_vm->RegisterFunction("GetComposeState", "CSXCaptureNative", GetComposeState);
		a_vm->RegisterFunction("GetComposeStatus", "CSXCaptureNative", GetComposeStatus);
		return true;
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (a_message && (a_message->type == SKSE::MessagingInterface::kPostLoad ||
		                  a_message->type == SKSE::MessagingInterface::kDataLoaded))
			(void)ConnectToCSX();
	}

	void InitializeLogging()
	{
		auto directory = SKSE::log::log_directory();
		if (!directory)
			return;
		*directory /= "CSXCaptureCompanion.log";
		auto logger = std::make_shared<spdlog::logger>(
			"global log",
			std::make_shared<spdlog::sinks::basic_file_sink_mt>(directory->string(), true));
		spdlog::set_default_logger(std::move(logger));
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		spdlog::set_level(spdlog::level::info);
		spdlog::flush_on(spdlog::level::info);
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLogging();
	SKSE::Init(a_skse);
	CSXCaptureCompanion::SetNotificationCallback(ShowInGameNotification);
	SKSE::GetPapyrusInterface()->Register(RegisterPapyrus);
	SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);
	SKSE::log::info("CSX Capture Companion loaded");
	return true;
}
