#include "CaptureRuntime.h"

#include <utility>

namespace CSXCaptureCompanion
{
	CaptureRuntime::CaptureRuntime(ScreenshotDispatch a_dispatch, CaptureNotification a_notify) :
		capture(
			std::move(a_dispatch),
			[this](const std::filesystem::path& a_manifest, std::string a_requestId) {
				return composer.Queue(a_manifest, std::move(a_requestId));
			},
			std::move(a_notify))
	{}

	CaptureRuntime::~CaptureRuntime() = default;

	CaptureController& CaptureRuntime::Capture() noexcept
	{
		return capture;
	}

	VideoComposer& CaptureRuntime::Composer() noexcept
	{
		return composer;
	}
}
