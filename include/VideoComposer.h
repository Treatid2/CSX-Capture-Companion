#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace CSXCaptureCompanion
{
	using NotificationCallback = void (*)(std::string);
	void SetNotificationCallback(NotificationCallback a_callback) noexcept;
	void ShowNotification(std::string a_message);

	enum class ComposeState : std::int32_t
	{
		kIdle = 0,
		kQueued = 1,
		kEncoding = 2,
		kComplete = 3,
		kFailed = 4
	};

	class VideoComposer final
	{
	public:
		static VideoComposer& GetSingleton();

		bool Queue(const std::filesystem::path& a_sequenceDirectory);
		[[nodiscard]] ComposeState GetState() const noexcept;
		[[nodiscard]] std::string GetStatusText() const;

	private:
		VideoComposer() = default;
		~VideoComposer() = default;
		VideoComposer(const VideoComposer&) = delete;
		VideoComposer(VideoComposer&&) = delete;
		VideoComposer& operator=(const VideoComposer&) = delete;
		VideoComposer& operator=(VideoComposer&&) = delete;

		void Run(std::filesystem::path a_sequenceDirectory);
		void SetStatus(ComposeState a_state, std::string a_text);

		std::atomic<ComposeState> state{ ComposeState::kIdle };
		mutable std::mutex statusMutex;
		std::string statusText{ "No video composition has been requested." };
		std::jthread worker;
	};
}
