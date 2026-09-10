#include "CaptureSession.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace CSXCaptureCompanion
{
	using json = nlohmann::json;

	namespace
	{
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
			if (a_state == "stop_requested" || a_state == "cancel_requested" || a_state == "finalizing")
				return 2;
			if (a_state == "completed" || a_state == "completed_with_warnings" || a_state == "stopped")
				return 3;
			if (a_state == "failed" || a_state == "failed_partial" || a_state == "rejected" ||
				a_state == "cancelled" || a_state == "cancelled_partial" || a_state == "dropped")
				return 4;
			return a_state == "accepted" || a_state == "waiting_source" ||
			       a_state == "staged" || a_state == "queued" ||
			       a_state == "encoding" || a_state == "running" ? 1 : -1;
		}

		bool TryDecodeReceipt(
			const json& a_response,
			std::string_view a_expectedRequestId,
			std::string& a_state,
			std::filesystem::path& a_manifest,
			bool& a_hasManifest) noexcept
		{
			try {
				if (!IsSuccessfulResponse(a_response) || !a_response.contains("result") ||
					!a_response["result"].is_object()) {
					return false;
				}
				const auto& receipt = a_response["result"];
				if (!receipt.contains("requestId") || !receipt["requestId"].is_string() ||
					receipt["requestId"].get_ref<const std::string&>() != a_expectedRequestId ||
					!receipt.contains("state") || !receipt["state"].is_string()) {
					return false;
				}

				a_state = receipt["state"].get<std::string>();
				if (CaptureStateCode(a_state) < 0)
					return false;
				a_hasManifest = false;
				if (receipt.contains("manifest")) {
					const auto& manifest = receipt["manifest"];
					if (!manifest.is_object() || !manifest.contains("finalPath")) {
						return false;
					}
					if (manifest["finalPath"].is_string()) {
						const auto& finalPath = manifest["finalPath"].get_ref<const std::string&>();
						if (finalPath.empty())
							return false;
						a_manifest = std::filesystem::u8path(finalPath);
						a_hasManifest = true;
					} else if (!manifest["finalPath"].is_null()) {
						return false;
					}
				}
				if ((a_state == "completed" || a_state == "completed_with_warnings" || a_state == "stopped") &&
					!a_hasManifest)
					return false;
				return true;
			} catch (...) {
				return false;
			}
		}
	}

	bool IsSuccessfulResponse(const json& a_response) noexcept
	{
		try {
			return a_response.is_object() && a_response.contains("ok") &&
			       a_response["ok"].is_boolean() && a_response["ok"].get<bool>();
		} catch (...) {
			return false;
		}
	}

	std::string AcceptedRequestId(const json& a_response) noexcept
	{
		try {
			if (!IsSuccessfulResponse(a_response) || !a_response.contains("result") ||
				!a_response["result"].is_object()) {
				return {};
			}
			const auto& result = a_response["result"];
			if (!result.contains("requestId") || !result["requestId"].is_string())
				return {};
			return result["requestId"].get<std::string>();
		} catch (...) {
			return {};
		}
	}

	CaptureSession::CaptureSession(ScreenshotDispatch a_dispatch) :
		dispatch(std::move(a_dispatch))
	{}

	std::int32_t CaptureSession::Refresh()
	{
		std::lock_guard operationLock(operationMutex);
		return RefreshLocked();
	}

	std::int32_t CaptureSession::RefreshLocked()
	{
		std::string requestId;
		{
			std::lock_guard stateLock(stateMutex);
			requestId = activeRequestId;
			if (requestId.empty())
				return lastState;
		}

		const auto response = dispatch({ { "action", "request_get" }, { "requestId", requestId } });
		std::string state;
		std::filesystem::path manifest;
		bool hasManifest = false;
		if (!TryDecodeReceipt(response, requestId, state, manifest, hasManifest))
			return -1;

		const auto code = CaptureStateCode(state);
		std::lock_guard stateLock(stateMutex);
		if (activeRequestId != requestId)
			return lastState;
		if (IsTerminal(state)) {
			if (hasManifest)
				latestManifest = std::move(manifest);
			activeRequestId.clear();
		}
		lastState = code;
		return code;
	}

	bool CaptureSession::Toggle(json a_startRequest)
	{
		std::lock_guard operationLock(operationMutex);
		(void)RefreshLocked();

		std::string requestId;
		{
			std::lock_guard stateLock(stateMutex);
			requestId = activeRequestId;
		}

		if (!requestId.empty()) {
			const auto response = dispatch({ { "action", "sequence_stop" }, { "requestId", requestId } });
			std::string state;
			std::filesystem::path manifest;
			bool hasManifest = false;
			if (!TryDecodeReceipt(response, requestId, state, manifest, hasManifest))
				return false;
			const auto code = CaptureStateCode(state);
			std::lock_guard stateLock(stateMutex);
			if (activeRequestId != requestId)
				return false;
			if (IsTerminal(state)) {
				if (hasManifest)
					latestManifest = std::move(manifest);
				activeRequestId.clear();
			}
			lastState = code;
			return true;
		}

		const auto response = dispatch(std::move(a_startRequest));
		requestId = AcceptedRequestId(response);
		if (requestId.empty())
			return false;

		std::lock_guard stateLock(stateMutex);
		activeRequestId = std::move(requestId);
		lastState = 1;
		return true;
	}

	std::filesystem::path CaptureSession::LatestManifest() const
	{
		std::lock_guard stateLock(stateMutex);
		return latestManifest;
	}

	std::string CaptureSession::ActiveRequestId() const
	{
		std::lock_guard stateLock(stateMutex);
		return activeRequestId;
	}
}
