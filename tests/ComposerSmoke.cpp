#include "VideoComposer.h"

#include <chrono>
#include <iostream>
#include <thread>

int wmain(int a_argumentCount, wchar_t** a_arguments)
{
	if (a_argumentCount != 2) {
		std::cerr << "Usage: CSXCaptureComposerSmoke <sequence-directory>\n";
		return 2;
	}

	auto& composer = CSXCaptureCompanion::VideoComposer::GetSingleton();
	if (!composer.Queue(a_arguments[1])) {
		std::cerr << "Composer rejected the smoke-test sequence.\n";
		return 3;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (std::chrono::steady_clock::now() < deadline) {
		const auto state = composer.GetState();
		if (state == CSXCaptureCompanion::ComposeState::kComplete) {
			std::cout << composer.GetStatusText() << '\n';
			return 0;
		}
		if (state == CSXCaptureCompanion::ComposeState::kFailed) {
			std::cerr << composer.GetStatusText() << '\n';
			return 4;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}

	std::cerr << "Composer smoke test timed out.\n";
	return 5;
}
