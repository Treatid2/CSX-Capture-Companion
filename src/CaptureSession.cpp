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

		ReceiptFailure TryDecodeReceipt(const json& a_response,
			std::string_view a_expectedRequestId,
			std::string& a_state,
			std::filesystem::path& a_manifest,
			bool& a_hasManifest) noexcept
		{
			try {
				if (!IsSuccessfulResponse(a_response)) {
					if (a_response.is_object() && a_response.contains("error") &&
						a_response["error"].is_object() &&
						a_response["error"].value("code", std::string{}) ==
							"request_not_found") {
						return ReceiptFailure::kPermanent;
					}
					return ReceiptFailure::kTransient;
				}
				if (!a_response.contains("result") || !a_response["result"].is_object()) {
					return ReceiptFailure::kInvalid;
				}
				const auto& receipt = a_response["result"];
				if (!receipt.contains("requestId") || !receipt["requestId"].is_string() ||
					receipt["requestId"].get_ref<const std::string&>() !=
						a_expectedRequestId ||
					!receipt.contains("state") || !receipt["state"].is_string()) {
					return ReceiptFailure::kInvalid;
				}

				a_state = receipt["state"].get<std::string>();
				if (CaptureStateCode(a_state) < 0)
					return ReceiptFailure::kInvalid;
				a_hasManifest = false;
				if (receipt.contains("manifest")) {
					const auto& manifest = receipt["manifest"];
					if (!manifest.is_object() || !manifest.contains("finalPath")) {
						return ReceiptFailure::kInvalid;
					}
					if (manifest["finalPath"].is_string()) {
						const auto& finalPath =
							manifest["finalPath"].get_ref<const std::string&>();
						if (finalPath.empty())
							return ReceiptFailure::kInvalid;
						a_manifest = std::filesystem::u8path(finalPath);
						a_hasManifest = true;
					} else if (!manifest["finalPath"].is_null()) {
						return ReceiptFailure::kInvalid;
					}
				}
				if ((a_state == "completed" || a_state == "completed_with_warnings" ||
						a_state == "stopped") &&
					!a_hasManifest)
					return ReceiptFailure::kInvalid;
				return ReceiptFailure::kNone;
			} catch (...) {
				return ReceiptFailure::kInvalid;
			}
		}
	}  // namespace

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

	std::int32_t CaptureSession::Refresh() { return RefreshUpdate().state; }

	CaptureUpdate CaptureSession::RefreshUpdate()
	{
		std::lock_guard operationLock(operationMutex);
		return RefreshLocked();
	}

	CaptureUpdate CaptureSession::RefreshLocked()
	{
		std::string requestId;
		{
			std::lock_guard stateLock(stateMutex);
			requestId = activeRequestId;
			if (requestId.empty())
				return { .state = lastState };
		}
		CaptureUpdate update{ .requestId = requestId };

		const auto response =
			dispatch({ { "action", "request_get" }, { "requestId", requestId } });
		std::string state;
		std::filesystem::path manifest;
		bool hasManifest = false;
		update.failure =
			TryDecodeReceipt(response, requestId, state, manifest, hasManifest);
		if (update.failure != ReceiptFailure::kNone)
			return update;

		const auto code = CaptureStateCode(state);
		update.state = code;
		update.accepted = true;
		update.terminal = IsTerminal(state);
		update.hasManifest = hasManifest;
		if (hasManifest)
			update.manifest = manifest;
		std::lock_guard stateLock(stateMutex);
		if (activeRequestId != requestId)
			return { .state = lastState };
		if (update.terminal) {
			if (hasManifest)
				latestManifest = std::move(manifest);
			activeRequestId.clear();
		}
		lastState = code;
		return update;
	}

	bool CaptureSession::Toggle(json a_startRequest)
	{
		return ToggleUpdate(std::move(a_startRequest)).accepted;
	}

	CaptureUpdate CaptureSession::ToggleUpdate(json a_startRequest)
	{
		std::lock_guard operationLock(operationMutex);
		bool hadActiveRequest = false;
		{
			std::lock_guard stateLock(stateMutex);
			hadActiveRequest = !activeRequestId.empty();
		}
		auto refresh = RefreshLocked();
		if (refresh.terminal)
			return refresh;

		std::string requestId;
		{
			std::lock_guard stateLock(stateMutex);
			requestId = activeRequestId;
		}

		if (!requestId.empty()) {
			const auto response =
				dispatch({ { "action", "sequence_stop" }, { "requestId", requestId } });
			std::string state;
			std::filesystem::path manifest;
			bool hasManifest = false;
			CaptureUpdate update{ .requestId = requestId };
			update.failure =
				TryDecodeReceipt(response, requestId, state, manifest, hasManifest);
			if (update.failure != ReceiptFailure::kNone)
				return update;
			const auto code = CaptureStateCode(state);
			update.state = code;
			update.accepted = true;
			update.terminal = IsTerminal(state);
			update.hasManifest = hasManifest;
			if (hasManifest)
				update.manifest = manifest;
			std::lock_guard stateLock(stateMutex);
			if (activeRequestId != requestId)
				return {};
			if (update.terminal) {
				if (hasManifest)
					latestManifest = std::move(manifest);
				activeRequestId.clear();
			}
			lastState = code;
			return update;
		}
		if (hadActiveRequest)
			return refresh;

		const auto response = dispatch(std::move(a_startRequest));
		requestId = AcceptedRequestId(response);
		if (requestId.empty())
			return {};

		std::lock_guard stateLock(stateMutex);
		activeRequestId = std::move(requestId);
		lastState = 1;
		return { .requestId = activeRequestId, .state = 1, .accepted = true };
	}

	std::string CaptureSession::AbandonActiveRequest()
	{
		std::lock_guard operationLock(operationMutex);
		std::lock_guard stateLock(stateMutex);
		auto abandoned = std::move(activeRequestId);
		activeRequestId.clear();
		lastState = 0;
		return abandoned;
	}

	void CaptureSession::MarkUnavailable()
	{
		std::lock_guard stateLock(stateMutex);
		lastState = -1;
	}

	std::int32_t CaptureSession::CachedState() const
	{
		std::lock_guard stateLock(stateMutex);
		return lastState;
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
