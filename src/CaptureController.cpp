#include "CaptureController.h"

#include <SKSE/SKSE.h>

#include <chrono>
#include <optional>
#include <utility>

namespace CSXCaptureCompanion
{
	using json = nlohmann::json;
	using namespace std::chrono_literals;
	constexpr std::size_t kMaximumScreenshotPollFailures = 40;

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

		bool IsSuccessfulTerminal(std::string_view a_state)
		{
			return a_state == "completed" || a_state == "completed_with_warnings";
		}
	}

	CaptureController::CaptureController(
		ScreenshotDispatch a_dispatch,
		ComposeRequest a_compose,
		CaptureNotification a_notify) :
		dispatch(std::move(a_dispatch)),
		compose(std::move(a_compose)),
		notify(std::move(a_notify)),
		session(dispatch),
		worker([this] { Run(); })
	{}

	CaptureController::~CaptureController()
	{
		{
			std::lock_guard lock(queueMutex);
			stopping = true;
		}
		queueCondition.notify_one();
	}

	bool CaptureController::QueueScreenshot(json a_request)
	{
		return Enqueue({ CommandKind::kScreenshot, std::move(a_request) });
	}

	bool CaptureController::QueueToggle(json a_startRequest)
	{
		return Enqueue({ CommandKind::kToggle, std::move(a_startRequest) });
	}

	bool CaptureController::QueueCompose()
	{
		return Enqueue({ CommandKind::kCompose, json::object() });
	}

	std::int32_t CaptureController::CaptureState() const
	{
		return session.CachedState();
	}

	bool CaptureController::Enqueue(Command a_command)
	{
		{
			std::lock_guard lock(queueMutex);
			if (stopping || commands.size() >= kMaximumQueuedCommands)
				return false;
			commands.push_back(std::move(a_command));
		}
		queueCondition.notify_one();
		return true;
	}

	void CaptureController::Run()
	{
		std::optional<std::chrono::steady_clock::time_point> nextReceiptPoll;
		for (;;) {
			Command command;
			bool hasCommand = false;
			{
				std::unique_lock lock(queueMutex);
				if (HasPollableReceipts() && !nextReceiptPoll)
					nextReceiptPoll = std::chrono::steady_clock::now() + 250ms;
				if (!HasPollableReceipts())
					nextReceiptPoll.reset();
				if (nextReceiptPoll) {
					queueCondition.wait_until(lock, *nextReceiptPoll, [this] {
						return stopping || !commands.empty();
					});
				} else {
					queueCondition.wait(lock,
						[this] { return stopping || !commands.empty(); });
				}
				if (stopping)
					return;
				if (!commands.empty()) {
					command = std::move(commands.front());
					commands.pop_front();
					hasCommand = true;
				}
			}

			if (hasCommand)
				Process(std::move(command));

			if (nextReceiptPoll &&
				std::chrono::steady_clock::now() >= *nextReceiptPoll) {
				PollCapture();
				PollScreenshots();
				nextReceiptPoll =
					HasPollableReceipts() ? std::optional(std::chrono::steady_clock::now() + 250ms) : std::nullopt;
			}
		}
	}

	void CaptureController::Process(Command a_command)
	{
		switch (a_command.kind) {
		case CommandKind::kScreenshot:
			{
				if (pendingScreenshots.size() >= kMaximumPendingScreenshots) {
					SKSE::log::warn("Screenshot receipt capacity is full");
					notify("Screenshot queue is full; wait for completion");
					break;
				}
				auto requestId = AcceptedRequestId(dispatch(std::move(a_command.request)));
				if (requestId.empty()) {
					SKSE::log::warn("Screenshot request was not accepted");
					notify("Screenshot request failed");
				} else {
					pendingScreenshots.push_back({ std::move(requestId), 0 });
					notify("Screenshot queued");
				}
				break;
			}
		case CommandKind::kToggle:
			{
				if (sequenceReceiptUnavailable && !session.ActiveRequestId().empty()) {
					sequenceReceiptUnavailable = false;
					sequencePollFailures = 0;
					if (unavailableFailure == ReceiptFailure::kPermanent) {
						(void)session.AbandonActiveRequest();
						unavailableFailure = ReceiptFailure::kNone;
						notify("Capture record is no longer retained; trigger again to start");
						break;
					}
					notify("Retrying capture status");
				}
				const auto update = session.ToggleUpdate(std::move(a_command.request));
				if (!update.accepted) {
					SKSE::log::warn("Frame-capture toggle was not accepted");
					notify("Frame capture command failed");
				}
				HandleCaptureUpdate(update);
				break;
			}
		case CommandKind::kCompose:
			if (const auto active = session.ActiveRequestId(); !active.empty()) {
				pendingComposeRequestId = active;
				notify("Composition queued until capture finishes");
				PollCapture();
			} else if (!session.LatestManifest().empty()) {
				pendingComposeRequestId.clear();
				if (!compose(session.LatestManifest()))
					SKSE::log::warn("Video composition request was not accepted");
			} else {
				SKSE::log::warn(
					"Compose requested without a completed Screenshot API "
					"sequence receipt");
				notify("No completed capture is ready");
			}
			break;
		}
	}

	void CaptureController::PollScreenshots()
	{
		for (auto request = pendingScreenshots.begin(); request != pendingScreenshots.end();) {
			const auto response = dispatch({ { "action", "request_get" }, { "requestId", request->requestId } });
			if (!IsSuccessfulResponse(response) || !response.contains("result") ||
				!response["result"].is_object()) {
				if (++request->failedPolls >= kMaximumScreenshotPollFailures) {
					SKSE::log::warn("Screenshot {} receipt could not be refreshed", request->requestId);
					notify("Screenshot status unavailable - see CSXCaptureCompanion.log");
					request = pendingScreenshots.erase(request);
					continue;
				}
				++request;
				continue;
			}
			const auto& receipt = response["result"];
			if (!receipt.contains("requestId") || !receipt["requestId"].is_string() ||
				receipt["requestId"].get_ref<const std::string&>() != request->requestId ||
				!receipt.contains("state") || !receipt["state"].is_string()) {
				if (++request->failedPolls >= kMaximumScreenshotPollFailures) {
					SKSE::log::warn("Screenshot {} returned invalid receipt data", request->requestId);
					notify("Screenshot status unavailable - see CSXCaptureCompanion.log");
					request = pendingScreenshots.erase(request);
				} else {
					++request;
				}
				continue;
			}
			request->failedPolls = 0;
			const auto& state = receipt["state"].get_ref<const std::string&>();
			if (!IsTerminal(state)) {
				++request;
				continue;
			}
			if (IsSuccessfulTerminal(state)) {
				SKSE::log::info("Screenshot {} completed", request->requestId);
				notify("Screenshot saved");
			} else {
				SKSE::log::warn("Screenshot {} finished with state {}", request->requestId, state);
				notify("Screenshot failed - see CSXCaptureCompanion.log");
			}
			request = pendingScreenshots.erase(request);
		}
	}

	void CaptureController::PollCapture()
	{
		if (session.ActiveRequestId().empty() || sequenceReceiptUnavailable)
			return;
		HandleCaptureUpdate(session.RefreshUpdate());
	}

	void CaptureController::HandleCaptureUpdate(const CaptureUpdate& a_update)
	{
		if (a_update.failure != ReceiptFailure::kNone) {
			++sequencePollFailures;
			const bool exhausted = a_update.failure == ReceiptFailure::kPermanent ||
			                       sequencePollFailures >= kMaximumSequencePollFailures;
			if (!exhausted)
				return;
			sequenceReceiptUnavailable = true;
			unavailableFailure = a_update.failure;
			session.MarkUnavailable();
			SKSE::log::warn(
				"Capture {} receipt custody is unavailable after {} failed refreshes",
				a_update.requestId, sequencePollFailures);
			notify("Capture status unavailable; trigger capture to reset tracking");
			if (pendingComposeRequestId == a_update.requestId) {
				pendingComposeRequestId.clear();
				notify("Composition failed because capture status is unavailable");
			}
			return;
		}
		sequencePollFailures = 0;
		sequenceReceiptUnavailable = false;
		unavailableFailure = ReceiptFailure::kNone;
		if (a_update.terminal)
			TryComposePending(a_update);
	}

	void CaptureController::TryComposePending(const CaptureUpdate& a_update)
	{
		if (pendingComposeRequestId.empty() ||
			pendingComposeRequestId != a_update.requestId || !a_update.terminal)
			return;
		pendingComposeRequestId.clear();
		if (!a_update.hasManifest) {
			notify("Capture finished without a composable manifest");
			return;
		}
		if (!compose(a_update.manifest))
			SKSE::log::warn("Deferred video composition request was not accepted");
	}

	bool CaptureController::HasPollableReceipts() const
	{
		return (!sequenceReceiptUnavailable && !session.ActiveRequestId().empty()) ||
		       !pendingScreenshots.empty();
	}
}
