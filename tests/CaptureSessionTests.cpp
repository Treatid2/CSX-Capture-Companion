#include "CaptureSession.h"

#include <atomic>
#include <barrier>
#include <condition_variable>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
	using CSXCaptureCompanion::CaptureSession;
	using json = nlohmann::json;

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

	bool TestMalformedReplies()
	{
		const std::vector<json> malformed = {
			json::array(),
			{ { "ok", "yes" } },
			{ { "ok", true }, { "result", 7 } },
			{ { "ok", true }, { "result", { { "requestId", 9 }, { "state", "active" } } } },
			{ { "ok", true }, { "result", { { "requestId", "A" }, { "state", 9 } } } },
			{ { "ok", true }, { "result", { { "requestId", "A" }, { "state", "completed" }, { "manifest", json::object() } } } },
		};

		bool passed = true;
		for (const auto& reply : malformed) {
			bool started = false;
			CaptureSession session([&](json request) {
				const auto action = request.at("action").get<std::string>();
				if (action == "sequence_start") {
					started = true;
					return json{ { "ok", true }, { "result", { { "requestId", "A" } } } };
				}
				return reply;
			});
			passed &= Check(session.Toggle(StartRequest()) && started, "Could not establish malformed-reply fixture state.");
			passed &= Check(session.Refresh() == -1, "Malformed receipt did not report an unavailable state.");
			passed &= Check(session.ActiveRequestId() == "A", "Malformed receipt changed active request ownership.");
		}
		return passed;
	}

	bool TestAcceptedRequestReplies()
	{
		const std::vector<json> rejected = {
			json::array(),
			{ { "ok", true } },
			{ { "ok", true }, { "result", 7 } },
			{ { "ok", true }, { "result", { { "requestId", 9 } } } },
			{ { "ok", true }, { "result", { { "requestId", "" } } } },
		};
		bool passed = true;
		for (const auto& reply : rejected) {
			passed &= Check(CSXCaptureCompanion::AcceptedRequestId(reply).empty(),
				"A malformed acceptance reply produced a request ID.");
		}
		return passed && Check(
			CSXCaptureCompanion::AcceptedRequestId(
				{ { "ok", true }, { "result", { { "requestId", "accepted-A" } } } }) == "accepted-A",
			"A valid acceptance reply did not produce its request ID.");
	}

	bool TestConcurrentToggle()
	{
		std::atomic_int starts{ 0 };
		std::atomic_int stops{ 0 };
		CaptureSession session([&](json request) {
			const auto action = request.at("action").get<std::string>();
			if (action == "sequence_start") {
				++starts;
				return json{ { "ok", true }, { "result", { { "requestId", "A" } } } };
			}
			if (action == "request_get")
				return json{ { "ok", true }, { "result", { { "requestId", "A" }, { "state", "capturing" } } } };
			++stops;
			return json{
				{ "ok", true },
				{ "result", {
					{ "requestId", "A" },
					{ "state", "stop_requested" },
					{ "manifest", { { "finalPath", nullptr } } },
				} },
			};
		});

		std::barrier gate(3);
		std::atomic_int accepted{ 0 };
		auto toggle = [&] {
			gate.arrive_and_wait();
			if (session.Toggle(StartRequest()))
				++accepted;
		};
		std::jthread first(toggle);
		std::jthread second(toggle);
		gate.arrive_and_wait();
		first.join();
		second.join();

		return Check(accepted == 2, "A serialized start/stop pair did not complete.") &&
		       Check(starts == 1, "Concurrent toggles admitted more than one sequence start.") &&
		       Check(stops == 1, "Concurrent toggles did not direct the successor operation to the active request.") &&
		       Check(session.ActiveRequestId() == "A", "A stop request retired ownership before its terminal receipt.");
	}

	bool TestRefreshThenSuccessorStart()
	{
		std::mutex replyMutex;
		std::condition_variable replyCondition;
		bool refreshEntered = false;
		bool releaseRefresh = false;
		std::atomic_int starts{ 0 };
		CaptureSession session([&](json request) {
			const auto action = request.at("action").get<std::string>();
			if (action == "sequence_start") {
				const auto ordinal = ++starts;
				return json{ { "ok", true }, { "result", { { "requestId", ordinal == 1 ? "A" : "B" } } } };
			}
			if (action == "request_get") {
				std::unique_lock lock(replyMutex);
				refreshEntered = true;
				replyCondition.notify_all();
				replyCondition.wait(lock, [&] { return releaseRefresh; });
				return json{
					{ "ok", true },
					{ "result", {
						{ "requestId", "A" },
						{ "state", "completed" },
						{ "manifest", { { "finalPath", "D:/captures/A/sequence.json" } } },
					} },
				};
			}
			return json{ { "ok", false } };
		});

		if (!Check(session.Toggle(StartRequest()), "Could not establish the first capture request."))
			return false;
		std::jthread refresh([&] { (void)session.Refresh(); });
		{
			std::unique_lock lock(replyMutex);
			replyCondition.wait(lock, [&] { return refreshEntered; });
		}
		std::jthread successor([&] { (void)session.Toggle(StartRequest()); });
		{
			std::lock_guard lock(replyMutex);
			releaseRefresh = true;
		}
		replyCondition.notify_all();
		refresh.join();
		successor.join();

		return Check(starts == 2, "The terminal receipt did not permit one successor start.") &&
		       Check(session.ActiveRequestId() == "B", "The earlier receipt cleared the successor request.") &&
		       Check(session.LatestManifest() == std::filesystem::path("D:/captures/A/sequence.json"),
			       "The completed manifest was not retained.");
	}
}

int main()
{
	const auto passed = TestMalformedReplies() && TestAcceptedRequestReplies() &&
	                    TestConcurrentToggle() && TestRefreshThenSuccessorStart();
	return passed ? 0 : 1;
}
