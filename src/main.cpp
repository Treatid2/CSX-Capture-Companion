#include "CSXCaptureAPI.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>

namespace
{
	std::atomic<CSPluginAPI::ICSCaptureInterface001*> g_capture{ nullptr };
	std::atomic g_eye{ CSPluginAPI::CaptureEye001::kLeft };

	bool ConnectToCSX()
	{
		CSPluginAPI::CSMessage request{};
		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging || !messaging->Dispatch(
				CSPluginAPI::CSMessage::kMessage_GetInterface,
				&request,
				sizeof(request),
				CSPluginAPI::CSPluginName) ||
			!request.GetApiFunction) {
			SKSE::log::warn("CSX capture API discovery failed");
			return false;
		}

		auto* api = static_cast<CSPluginAPI::ICSInterface001*>(
			request.GetApiFunction(CSPluginAPI::CSInterfaceRevision005));
		if (!api || api->getBuildNumber() < CSPluginAPI::MinimumCaptureBuild) {
			SKSE::log::warn("CSX build 12 or later is required");
			return false;
		}

		auto* capture = api->GetCaptureInterface001();
		g_capture.store(capture, std::memory_order_release);
		SKSE::log::info("Connected to CSX lossless capture API");
		return capture != nullptr;
	}

	CSPluginAPI::CaptureEye001 SanitizeEye(std::int32_t a_eye)
	{
		switch (a_eye) {
		case 1:
			return CSPluginAPI::CaptureEye001::kRight;
		case 2:
			return CSPluginAPI::CaptureEye001::kBoth;
		default:
			return CSPluginAPI::CaptureEye001::kLeft;
		}
	}

	bool IsAvailable(RE::StaticFunctionTag*)
	{
		return g_capture.load(std::memory_order_acquire) != nullptr;
	}

	void SetEye(RE::StaticFunctionTag*, std::int32_t a_eye)
	{
		g_eye.store(SanitizeEye(a_eye), std::memory_order_release);
	}

	bool TakeScreenshot(RE::StaticFunctionTag*)
	{
		auto* capture = g_capture.load(std::memory_order_acquire);
		return capture && capture->RequestScreenshot(g_eye.load(std::memory_order_acquire)) ==
		                      CSPluginAPI::CaptureResult001::kSuccess;
	}

	bool ToggleFrameSequence(RE::StaticFunctionTag*)
	{
		auto* capture = g_capture.load(std::memory_order_acquire);
		if (!capture) {
			return false;
		}
		CSPluginAPI::CaptureStatus001 status{};
		if (capture->GetCaptureStatus(&status) != CSPluginAPI::CaptureResult001::kSuccess) {
			return false;
		}
		if (status.state == CSPluginAPI::CaptureState001::kCapturing) {
			return capture->StopFrameSequence() == CSPluginAPI::CaptureResult001::kSuccess;
		}
		if (status.state == CSPluginAPI::CaptureState001::kFlushing) {
			return false;
		}
		return capture->StartFrameSequence(g_eye.load(std::memory_order_acquire)) ==
		       CSPluginAPI::CaptureResult001::kSuccess;
	}

	std::int32_t GetCaptureState(RE::StaticFunctionTag*)
	{
		auto* capture = g_capture.load(std::memory_order_acquire);
		CSPluginAPI::CaptureStatus001 status{};
		if (!capture || capture->GetCaptureStatus(&status) != CSPluginAPI::CaptureResult001::kSuccess) {
			return -1;
		}
		return static_cast<std::int32_t>(status.state);
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("IsAvailable", "CSXCaptureNative", IsAvailable);
		a_vm->RegisterFunction("SetEye", "CSXCaptureNative", SetEye);
		a_vm->RegisterFunction("TakeScreenshot", "CSXCaptureNative", TakeScreenshot);
		a_vm->RegisterFunction("ToggleFrameSequence", "CSXCaptureNative", ToggleFrameSequence);
		a_vm->RegisterFunction("GetCaptureState", "CSXCaptureNative", GetCaptureState);
		return true;
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (a_message && (a_message->type == SKSE::MessagingInterface::kPostLoad ||
		                  a_message->type == SKSE::MessagingInterface::kDataLoaded)) {
			(void)ConnectToCSX();
		}
	}

	void InitializeLogging()
	{
		auto directory = SKSE::log::log_directory();
		if (!directory) {
			return;
		}
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
	SKSE::GetPapyrusInterface()->Register(RegisterPapyrus);
	SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);
	SKSE::log::info("CSX Capture Companion loaded");
	return true;
}
