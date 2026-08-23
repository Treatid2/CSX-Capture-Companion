#pragma once

#include <cstdint>

namespace CSPluginAPI
{
	inline constexpr const char* CSPluginName = "CommunityShaders";
	inline constexpr std::uint32_t CSInterfaceMessageType = 0x43534150;
	inline constexpr unsigned int CSInterfaceRevision005 = 5;
	inline constexpr unsigned int MinimumCaptureBuild = 12;

	struct CSMessage
	{
		enum : std::uint32_t
		{
			kMessage_GetInterface = CSInterfaceMessageType
		};
		void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
	};

	enum class CaptureEye001 : std::uint32_t
	{
		kLeft = 0,
		kRight = 1,
		kBoth = 2
	};

	enum class CaptureState001 : std::uint32_t
	{
		kIdle = 0,
		kCapturing = 1,
		kFlushing = 2,
		kComplete = 3,
		kFailed = 4
	};

	enum class CaptureResult001 : std::uint32_t
	{
		kSuccess = 0,
		kInvalidArgument = 1,
		kFeatureDisabled = 2,
		kBusy = 3,
		kNoActiveSequence = 4,
		kOutputUnavailable = 5,
		kInternalError = 6,
		kBufferTooSmall = 7
	};

	struct CaptureStatus001
	{
		std::uint32_t structSize = sizeof(CaptureStatus001);
		CaptureState001 state = CaptureState001::kIdle;
		CaptureEye001 eye = CaptureEye001::kLeft;
		std::uint64_t sessionId = 0;
		std::uint64_t framesScheduled = 0;
		std::uint64_t framesWritten = 0;
		std::uint64_t framesDropped = 0;
	};

	struct ICSCaptureInterface001
	{
		virtual CaptureResult001 RequestScreenshot(CaptureEye001 eye) = 0;
		virtual CaptureResult001 StartFrameSequence(CaptureEye001 eye) = 0;
		virtual CaptureResult001 StopFrameSequence() = 0;
		virtual CaptureResult001 GetCaptureStatus(CaptureStatus001* status) = 0;
		virtual CaptureResult001 CopySequencePath(
			std::uint64_t sessionId,
			char* buffer,
			std::uint32_t bufferBytes,
			std::uint32_t* requiredBytes) = 0;
	};

	// The capture getter is appended at revision 5. The preceding declarations
	// preserve the provider's established vtable layout.
	enum class UpscalePreset : std::uint32_t
	{
		kNativeAA = 0,
		kQuality = 1,
		kBalanced = 2,
		kPerformance = 3,
		kUltraPerformance = 4,
		kHoshipa = 5,
		kUltraQuality = 6
	};
	enum class DLSSProfile : std::uint32_t { kJ, kK, kL, kM, kF };
	enum class UpscaleMethod : std::uint32_t { kNone, kTAA, kFSR, kDLSS };
	enum class VRUpscalingTransitionProfileDecision : std::uint32_t { kBlocked, kNoChange, kApply };

	struct ICSInterface001
	{
		virtual unsigned int getBuildNumber() = 0;
		virtual bool GetSSSEnabled() = 0;
		virtual void SetSSSEnabled(bool enabled) = 0;
		virtual bool GetSSGIEnabled() = 0;
		virtual void SetSSGIEnabled(bool enabled) = 0;
		virtual bool GetVolumetricLightingExteriorEnabled() = 0;
		virtual void SetVolumetricLightingExteriorEnabled(bool enabled) = 0;
		virtual UpscalePreset GetUpscalePreset() = 0;
		virtual void SetUpscalePreset(UpscalePreset preset) = 0;
		virtual bool GetLightLimitFixContactShadowsEnabled() = 0;
		virtual void SetLightLimitFixContactShadowsEnabled(bool enabled) = 0;
		virtual DLSSProfile GetDLSSProfile() = 0;
		virtual void SetDLSSProfile(DLSSProfile profile) = 0;
		virtual bool GetRenderAtUpscaleResEnabled() = 0;
		virtual void SetRenderAtUpscaleResEnabled(bool enabled) = 0;
		virtual bool GetRenderAtUpscaleResActive() = 0;
		virtual void SetVRUpscalingTransitionProfile(bool enabled, UpscalePreset preset, DLSSProfile profile) = 0;
		virtual UpscaleMethod GetUpscaleMethod() = 0;
		virtual void SetUpscaleMethod(UpscaleMethod method) = 0;
		virtual void SetVRUpscalingTransitionProfileForMethod(UpscaleMethod method, bool enabled, UpscalePreset preset, DLSSProfile profile) = 0;
		virtual std::uint32_t GetVRUpscalingApplyBlockReasons() = 0;
		virtual bool IsVRUpscalingProfileApplyAllowed() = 0;
		virtual VRUpscalingTransitionProfileDecision GetVRUpscalingTransitionProfileDecision(
			UpscaleMethod method,
			bool enabled,
			UpscalePreset preset,
			DLSSProfile profile) = 0;
		virtual ICSCaptureInterface001* GetCaptureInterface001() = 0;
	};
}
