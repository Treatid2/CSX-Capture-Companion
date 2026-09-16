#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace CSXCaptureCompanion
{
	using ScreenshotDispatch = std::function<nlohmann::json(nlohmann::json)>;

	enum class ReceiptFailure
	{
		kNone,
		kTransient,
		kPermanent,
		kInvalid
	};

	struct CaptureUpdate
	{
		std::string requestId;
		std::filesystem::path manifest;
		std::int32_t state{ -1 };
		ReceiptFailure failure{ ReceiptFailure::kNone };
		bool accepted{ false };
		bool terminal{ false };
		bool hasManifest{ false };
	};

	[[nodiscard]] bool
	IsSuccessfulResponse(const nlohmann::json& a_response) noexcept;
	[[nodiscard]] std::string
	AcceptedRequestId(const nlohmann::json& a_response) noexcept;
	/// Map documented receipt states to active (1), stopping (2), complete (3),
	/// failed (4), or invalid (-1) for both still and sequence tracking.
	[[nodiscard]] std::int32_t CaptureStateCode(std::string_view a_state) noexcept;

	class CaptureSession final
	{
	public:
		explicit CaptureSession(ScreenshotDispatch a_dispatch);

		[[nodiscard]] std::int32_t Refresh();
		[[nodiscard]] bool Toggle(nlohmann::json a_startRequest);
		[[nodiscard]] CaptureUpdate RefreshUpdate();
		[[nodiscard]] CaptureUpdate ToggleUpdate(nlohmann::json a_startRequest);
		[[nodiscard]] std::string AbandonActiveRequest();
		void MarkUnavailable();
		[[nodiscard]] std::int32_t CachedState() const;
		/// Return the eligible terminal manifest of the latest accepted attempt.
		[[nodiscard]] std::filesystem::path LatestManifest() const;
		[[nodiscard]] std::string ActiveRequestId() const;

	private:
		CaptureUpdate RefreshLocked();

		ScreenshotDispatch dispatch;
		mutable std::mutex operationMutex;
		mutable std::mutex stateMutex;
		std::string activeRequestId;
		std::filesystem::path latestManifest;
		std::int32_t lastState{ 0 };
	};
}
