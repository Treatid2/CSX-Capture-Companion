#pragma once

#include "CaptureSession.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace CSXCaptureCompanion
{
	using ComposeRequest = std::function<bool(const std::filesystem::path&)>;
	using CaptureNotification = std::function<void(std::string)>;

	class CaptureController final
	{
	public:
		CaptureController(ScreenshotDispatch a_dispatch, ComposeRequest a_compose,
			CaptureNotification a_notify);
		~CaptureController();

		CaptureController(const CaptureController&) = delete;
		CaptureController(CaptureController&&) = delete;
		CaptureController& operator=(const CaptureController&) = delete;
		CaptureController& operator=(CaptureController&&) = delete;

		[[nodiscard]] bool QueueScreenshot(nlohmann::json a_request);
		[[nodiscard]] bool QueueToggle(nlohmann::json a_startRequest);
		[[nodiscard]] bool QueueCompose();
		[[nodiscard]] std::int32_t CaptureState() const;

	private:
		enum class CommandKind
		{
			kScreenshot,
			kToggle,
			kCompose
		};

		struct Command
		{
			CommandKind kind{};
			nlohmann::json request;
		};

		struct PendingScreenshot
		{
			std::string requestId;
			std::size_t failedPolls{};
		};

		[[nodiscard]] bool Enqueue(Command a_command);
		void Run();
		void Process(Command a_command);
		void PollCapture();
		void PollScreenshots();
		void HandleCaptureUpdate(const CaptureUpdate& a_update);
		void TryComposePending(const CaptureUpdate& a_update);
		[[nodiscard]] bool HasPollableReceipts() const;

		static constexpr std::size_t kMaximumQueuedCommands = 16;
		static constexpr std::size_t kMaximumPendingScreenshots = 16;
		static constexpr std::size_t kMaximumSequencePollFailures = 4;
		ScreenshotDispatch dispatch;
		ComposeRequest compose;
		CaptureNotification notify;
		CaptureSession session;
		mutable std::mutex queueMutex;
		std::condition_variable queueCondition;
		std::deque<Command> commands;
		std::deque<PendingScreenshot> pendingScreenshots;
		bool stopping{ false };
		std::string pendingComposeRequestId;
		std::size_t sequencePollFailures{ 0 };
		bool sequenceReceiptUnavailable{ false };
		ReceiptFailure unavailableFailure{ ReceiptFailure::kNone };
		std::jthread worker;
	};
}
