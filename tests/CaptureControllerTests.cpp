#include "CaptureController.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
	using CSXCaptureCompanion::CaptureController;
	using json = nlohmann::json;
	using namespace std::chrono_literals;

	json StartRequest()
	{
		return { { "action", "sequence_start" }, { "sequence", json::object() } };
	}

	bool Check(bool a_condition, std::string_view a_message)
	{
		if (!a_condition)
			std::cerr << a_message << '\n';
		return a_condition;
	}

	template <class Predicate>
	bool WaitFor(Predicate a_predicate, std::chrono::milliseconds a_timeout = 3s)
	{
		const auto deadline = std::chrono::steady_clock::now() + a_timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			if (a_predicate())
				return true;
			std::this_thread::sleep_for(10ms);
		}
		return a_predicate();
	}

	bool TestCallerNeverWaitsForDispatch()
	{
		std::atomic_bool dispatchEntered{ false };
		std::atomic_bool releaseDispatch{ false };
		CaptureController controller(
			[&](json) {
				dispatchEntered = true;
				while (!releaseDispatch.load())
					std::this_thread::sleep_for(5ms);
				return json{ { "ok", true }, { "result", { { "requestId", "still-A" } } } };
			},
			[](const std::filesystem::path&) { return true; },
			[](std::string) {});

		const auto started = std::chrono::steady_clock::now();
		const bool queued = controller.QueueScreenshot({ { "action", "capture" } });
		const auto elapsed = std::chrono::steady_clock::now() - started;
		const bool entered = WaitFor([&] { return dispatchEntered.load(); });
		releaseDispatch = true;
		return Check(queued, "The screenshot command was not queued.") &&
		       Check(elapsed < 100ms, "The screenshot caller waited for transport dispatch.") &&
		       Check(entered, "The queued screenshot did not reach the dispatch worker.");
	}

	bool TestScreenshotReceiptReportsCompletion()
	{
		std::atomic_int receiptPolls{ 0 };
		std::mutex notificationMutex;
		std::vector<std::string> notifications;
		CaptureController controller(
			[&](json request) {
				const auto action = request.at("action").get<std::string>();
				if (action == "capture")
					return json{ { "ok", true }, { "result", { { "requestId", "still-A" } } } };
				if (action == "request_get") {
					const auto state = receiptPolls++ == 0 ? "encoding" : "completed";
					return json{
						{ "ok", true },
						{ "result", { { "requestId", "still-A" }, { "state", state } } },
					};
				}
				return json{ { "ok", false } };
			},
			[](const std::filesystem::path&) { return true; },
			[&](std::string message) {
				std::lock_guard lock(notificationMutex);
				notifications.push_back(std::move(message));
			});

		if (!Check(controller.QueueScreenshot({ { "action", "capture" } }),
				"The screenshot command was not queued.")) {
			return false;
		}
		const bool completed = WaitFor([&] {
			std::lock_guard lock(notificationMutex);
			return std::ranges::find(notifications, "Screenshot saved") != notifications.end();
		});
		std::lock_guard lock(notificationMutex);
		return Check(completed, "The screenshot terminal receipt was not reported.") &&
		       Check(!notifications.empty() && notifications.front() == "Screenshot queued",
			       "The screenshot was not acknowledged immediately after API acceptance.");
	}

	bool TestStopReceiptIsPolledAndComposed()
	{
		std::atomic_bool stopRequested{ false };
		std::atomic_int transientRefreshes{ 0 };
		std::mutex composedMutex;
		std::filesystem::path composed;
		CaptureController controller(
			[&](json request) {
				const auto action = request.at("action").get<std::string>();
				if (action == "sequence_start")
					return json{ { "ok", true }, { "result", { { "requestId", "A" } } } };
				if (action == "sequence_stop") {
					stopRequested = true;
					return json{
						{ "ok", true },
						{ "result", {
							{ "requestId", "A" }, { "state", "stop_requested" },
							{ "manifest", { { "finalPath", nullptr } } },
						} },
					};
				}
				if (action == "request_get" && stopRequested.load()) {
					if (transientRefreshes++ == 0)
						return json{ { "ok", false }, { "error", { { "code", "transport_error" } } } };
					return json{
						{ "ok", true },
						{ "result", {
							{ "requestId", "A" }, { "state", "stopped" },
							{ "manifest", { { "finalPath", "D:/captures/A/sequence.json" } } },
						} },
					};
				}
				return json{ { "ok", true }, { "result", { { "requestId", "A" }, { "state", "running" } } } };
			},
			[&](const std::filesystem::path& a_manifest) {
				std::lock_guard lock(composedMutex);
				composed = a_manifest;
				return true;
			},
			[](std::string) {});

		if (!Check(controller.QueueToggle(StartRequest()), "The start command was not queued.") ||
			!Check(WaitFor([&] { return controller.CaptureState() == 1; }), "The sequence did not start.") ||
			!Check(controller.QueueToggle(StartRequest()), "The stop command was not queued.") ||
			!Check(controller.QueueCompose(), "The compose command was not queued.")) {
			return false;
		}

		const bool completed = WaitFor([&] {
			std::lock_guard lock(composedMutex);
			return !composed.empty();
		});
		std::lock_guard lock(composedMutex);
		return Check(completed, "The terminal stop receipt was not composed.") &&
		       Check(transientRefreshes >= 2, "A transient receipt failure was not retried.") &&
		       Check(composed == std::filesystem::path("D:/captures/A/sequence.json"),
			       "Composition received the wrong manifest path.");
	}
}

int main()
{
	return TestCallerNeverWaitsForDispatch() && TestScreenshotReceiptReportsCompletion() &&
	       TestStopReceiptIsPolledAndComposed() ? 0 : 1;
}
