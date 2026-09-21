#pragma once

#include "CaptureController.h"
#include "VideoComposer.h"

namespace CSXCaptureCompanion
{
	class CaptureRuntime final
	{
	public:
		CaptureRuntime(ScreenshotDispatch a_dispatch, CaptureNotification a_notify);
		~CaptureRuntime();

		CaptureRuntime(const CaptureRuntime&) = delete;
		CaptureRuntime(CaptureRuntime&&) = delete;
		CaptureRuntime& operator=(const CaptureRuntime&) = delete;
		CaptureRuntime& operator=(CaptureRuntime&&) = delete;

		[[nodiscard]] CaptureController& Capture() noexcept;
		[[nodiscard]] VideoComposer& Composer() noexcept;

	private:
		// Destruction is reverse declaration order: stop capture callbacks first.
		VideoComposer composer;
		CaptureController capture;
	};
}
