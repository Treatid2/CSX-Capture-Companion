#include "CaptureController.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
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

	bool TestToggleTerminalResolvesDeferredCompose()
	{
		std::atomic_int refreshes{ 0 };
		std::mutex composedMutex;
		std::vector<std::filesystem::path> composed;
		CaptureController controller(
			[&](json request) {
				const auto action = request.at("action").get<std::string>();
				if (action == "sequence_start")
					return json{ { "ok", true }, { "result", { { "requestId", "race" } } } };
				if (action == "request_get" && refreshes++ == 0)
					return json{
						{ "ok", true },
						{ "result", { { "requestId", "race" }, { "state", "running" } } }
					};
				if (action == "request_get") {
					return json{
						{ "ok", true },
						{ "result",
							{
								{ "requestId", "race" },
								{ "state", "stopped" },
								{ "manifest",
									{ { "finalPath", "D:/captures/race/sequence.json" } } },
							} },
					};
				}
				return json{ { "ok", false } };
			},
			[&](const std::filesystem::path& manifest) {
				std::lock_guard lock(composedMutex);
				composed.push_back(manifest);
				return true;
			},
			[](std::string) {});

		if (!Check(controller.QueueToggle(StartRequest()),
				"The race capture did not queue.") ||
			!Check(WaitFor([&] { return controller.CaptureState() == 1; }),
				"The race capture did not start.") ||
			!Check(controller.QueueCompose(),
				"The deferred compose did not queue.") ||
			!Check(controller.QueueToggle(StartRequest()),
				"The terminal toggle did not queue.")) {
			return false;
		}
		const bool completed = WaitFor([&] {
			std::lock_guard lock(composedMutex);
			return !composed.empty();
		});
		std::lock_guard lock(composedMutex);
		return Check(completed,
				   "A terminal receipt consumed by Toggle did not "
				   "resolve composition.") &&
		       Check(composed.size() == 1,
				   "The terminal toggle composed more than once.") &&
		       Check(composed.front() ==
						 std::filesystem::path("D:/captures/race/sequence.json"),
				   "The terminal toggle composed the wrong recording.");
	}

	bool TestDeferredComposeDoesNotUseOlderManifest()
	{
		std::atomic_int starts{ 0 };
		std::atomic_int current{ 0 };
		std::mutex resultMutex;
		std::vector<std::filesystem::path> composed;
		std::vector<std::string> notifications;
		CaptureController controller(
			[&](json request) {
				const auto action = request.at("action").get<std::string>();
				if (action == "sequence_start") {
					current = ++starts;
					return json{ { "ok", true },
						{ "result", { { "requestId", current == 1 ? "A" : "B" } } } };
				}
				if (action == "request_get" && request.at("requestId") == "A") {
					return json{
						{ "ok", true },
						{ "result",
							{
								{ "requestId", "A" },
								{ "state", "completed" },
								{ "manifest", { { "finalPath", "D:/captures/A/sequence.json" } } },
							} },
					};
				}
				if (action == "request_get")
					return json{ { "ok", true },
						{ "result", { { "requestId", "B" }, { "state", "failed" } } } };
				return json{ { "ok", false } };
			},
			[&](const std::filesystem::path& manifest) {
				std::lock_guard lock(resultMutex);
				composed.push_back(manifest);
				return true;
			},
			[&](std::string message) {
				std::lock_guard lock(resultMutex);
				notifications.push_back(std::move(message));
			});

		if (!Check(controller.QueueToggle(StartRequest()),
				"Capture A did not queue.") ||
			!Check(WaitFor([&] { return controller.CaptureState() == 3; }),
				"Capture A did not complete.") ||
			!Check(controller.QueueToggle(StartRequest()),
				"Capture B did not queue.") ||
			!Check(WaitFor([&] { return controller.CaptureState() == 1; }),
				"Capture B did not start.") ||
			!Check(controller.QueueCompose(),
				"Capture B composition did not queue.")) {
			return false;
		}
		const bool failedExplicitly = WaitFor([&] {
			std::lock_guard lock(resultMutex);
			return std::ranges::find(
					   notifications,
					   "Capture finished without a composable manifest") !=
			       notifications.end();
		});
		std::lock_guard lock(resultMutex);
		return Check(failedExplicitly,
				   "Capture B's missing manifest was not reported.") &&
		       Check(composed.empty(),
				   "Capture B incorrectly composed Capture A's retained manifest.");
	}

	bool TestPermanentReceiptFailureRequiresExplicitRecovery()
	{
		std::atomic_int receiptPolls{ 0 };
		std::mutex notificationMutex;
		std::vector<std::string> notifications;
		CaptureController controller(
			[&](json request) {
				if (request.at("action") == "sequence_start")
					return json{ { "ok", true }, { "result", { { "requestId", "lost" } } } };
				++receiptPolls;
				return json{ { "ok", false }, { "error", { { "code", "request_not_found" } } } };
			},
			[](const std::filesystem::path&) { return true; },
			[&](std::string message) {
				std::lock_guard lock(notificationMutex);
				notifications.push_back(std::move(message));
			});

		if (!Check(controller.QueueToggle(StartRequest()),
				"The lost capture did not queue.") ||
			!Check(WaitFor([&] { return controller.CaptureState() == -1; }),
				"Permanent loss did not become unavailable.")) {
			return false;
		}
		const auto pollsAtFailure = receiptPolls.load();
		std::this_thread::sleep_for(600ms);
		if (!Check(receiptPolls == pollsAtFailure,
				"Unavailable custody continued polling indefinitely.") ||
			!Check(controller.QueueToggle(StartRequest()),
				"Explicit recovery did not queue.") ||
			!Check(WaitFor([&] { return controller.CaptureState() == 0; }),
				"Explicit recovery did not clear custody.")) {
			return false;
		}
		std::lock_guard lock(notificationMutex);
		return Check(std::ranges::count(notifications,
						 "Capture status unavailable; trigger capture "
						 "to reset tracking") == 1,
				   "Permanent loss did not produce exactly one disposition.") &&
		       Check(std::ranges::find(notifications,
						 "Capture record is no longer retained; "
						 "trigger again to start") !=
						 notifications.end(),
				   "Permanent loss did not require a separate restart gesture.");
	}

	bool TestTransientReceiptBudgetCanRecover()
	{
		std::atomic_bool transportRecovered{ false };
		std::atomic_bool stopRequested{ false };
		std::atomic_int starts{ 0 };
		CaptureController controller(
			[&](json request) {
				const auto action = request.at("action").get<std::string>();
				if (action == "sequence_start") {
					++starts;
					return json{ { "ok", true }, { "result", { { "requestId", "transient" } } } };
				}
				if (!transportRecovered.load())
					return json{ { "ok", false }, { "error", { { "code", "transport_error" } } } };
				if (action == "sequence_stop") {
					stopRequested = true;
					return json{
						{ "ok", true },
						{ "result",
							{
								{ "requestId", "transient" },
								{ "state", "stop_requested" },
								{ "manifest", { { "finalPath", nullptr } } },
							} },
					};
				}
				if (stopRequested.load()) {
					return json{
						{ "ok", true },
						{ "result",
							{
								{ "requestId", "transient" },
								{ "state", "stopped" },
								{ "manifest",
									{ { "finalPath", "D:/captures/transient/sequence.json" } } },
							} },
					};
				}
				return json{
					{ "ok", true },
					{ "result", { { "requestId", "transient" }, { "state", "running" } } }
				};
			},
			[](const std::filesystem::path&) { return true; }, [](std::string) {});

		if (!Check(controller.QueueToggle(StartRequest()),
				"The transient capture did not queue.") ||
			!Check(WaitFor([&] { return controller.CaptureState() == -1; }, 3s),
				"Persistent transport failures did not exhaust their finite "
				"budget.")) {
			return false;
		}
		transportRecovered = true;
		return Check(controller.QueueToggle(StartRequest()),
				   "The transient recovery gesture did not queue.") &&
		       Check(WaitFor([&] { return controller.CaptureState() == 3; }),
				   "Recovered transport did not stop and finalize the owned "
				   "capture.") &&
		       Check(starts == 1,
				   "Transient recovery admitted a duplicate recording.");
	}

	bool TestCommandIngressDoesNotStarveReceipts()
	{
		std::mutex notificationMutex;
		std::vector<std::string> notifications;
		CaptureController controller(
			[](json request) {
				if (request.at("action") == "capture")
					return json{ { "ok", true }, { "result", { { "requestId", "fair" } } } };
				if (request.at("action") == "request_get") {
					return json{
						{ "ok", true },
						{ "result", { { "requestId", "fair" }, { "state", "completed" } } },
					};
				}
				return json{ { "ok", false } };
			},
			[](const std::filesystem::path&) { return true; },
			[&](std::string message) {
				if (message == "No completed capture is ready")
					std::this_thread::sleep_for(5ms);
				std::lock_guard lock(notificationMutex);
				notifications.push_back(std::move(message));
			});

		if (!Check(controller.QueueScreenshot({ { "action", "capture" } }),
				"The fairness screenshot did not queue.") ||
			!Check(WaitFor([&] {
				std::lock_guard lock(notificationMutex);
				return std::ranges::find(notifications, "Screenshot queued") !=
			           notifications.end();
			}),
				"The fairness screenshot was not accepted.")) {
			return false;
		}

		std::atomic_bool keepFlooding{ true };
		std::jthread producer([&] {
			while (keepFlooding.load()) {
				(void)controller.QueueCompose();
				std::this_thread::sleep_for(1ms);
			}
		});
		const bool completed = WaitFor(
			[&] {
				std::lock_guard lock(notificationMutex);
				return std::ranges::find(notifications, "Screenshot saved") !=
			           notifications.end();
			},
			2s);
		keepFlooding = false;
		producer.join();
		return Check(completed,
			"Continuous commands starved the screenshot receipt deadline.");
	}

	bool TestPendingScreenshotCapacityIsBounded()
	{
		std::atomic_int accepted{ 0 };
		std::mutex notificationMutex;
		std::vector<std::string> notifications;
		CaptureController controller(
			[&](json request) {
				if (request.at("action") == "capture") {
					const auto ordinal = ++accepted;
					return json{
						{ "ok", true },
						{ "result", { { "requestId", std::format("still-{}", ordinal) } } }
					};
				}
				return json{
					{ "ok", true },
					{ "result",
						{ { "requestId", request.at("requestId") }, { "state", "encoding" } } },
				};
			},
			[](const std::filesystem::path&) { return true; },
			[&](std::string message) {
				std::lock_guard lock(notificationMutex);
				notifications.push_back(std::move(message));
			});

		for (int i = 0; i < 16; ++i) {
			while (!controller.QueueScreenshot({ { "action", "capture" } }))
				std::this_thread::sleep_for(1ms);
		}
		if (!Check(WaitFor([&] { return accepted == 16; }),
				"The receipt-cap fixture did not fill."))
			return false;
		if (!Check(controller.QueueScreenshot({ { "action", "capture" } }),
				"The over-cap command did not queue."))
			return false;
		const bool rejected = WaitFor([&] {
			std::lock_guard lock(notificationMutex);
			return std::ranges::find(notifications,
					   "Screenshot queue is full; wait for completion") !=
			       notifications.end();
		});
		return Check(rejected,
				   "The pending screenshot receipt collection exceeded "
				   "its fixed capacity.") &&
		       Check(accepted == 16, "An over-cap screenshot reached the provider.");
	}
}

int main()
{
	return TestCallerNeverWaitsForDispatch() && TestScreenshotReceiptReportsCompletion() &&
	       TestStopReceiptIsPolledAndComposed() && TestToggleTerminalResolvesDeferredCompose() &&
	       TestDeferredComposeDoesNotUseOlderManifest() &&
	       TestPermanentReceiptFailureRequiresExplicitRecovery() &&
	       TestTransientReceiptBudgetCanRecover() &&
	       TestCommandIngressDoesNotStarveReceipts() &&
	       TestPendingScreenshotCapacityIsBounded() ? 0 : 1;
}
