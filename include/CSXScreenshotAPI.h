#pragma once

#include <cstdint>

namespace CSX::ScreenshotAPI
{
	inline constexpr char ServiceName[] = "csx.screenshot";
	inline constexpr std::uint32_t ServiceMajor = 1;
	inline constexpr std::uint32_t ServiceMinor = 0;
	inline constexpr std::uint32_t SchemaRevision = 1;

	enum class Status : std::uint32_t
	{
		kSuccess = 0,
		kInvalidArgument = 1,
		kStructureTooSmall = 2,
		kWrongThread = 3,
		kInvalidJson = 4,
		kServiceUnavailable = 5,
		kInternalError = 6
	};

	struct Request001
	{
		std::uint32_t structSize = sizeof(Request001);
		const char* jsonUtf8 = nullptr;
		std::uint32_t jsonBytes = 0;
	};

	struct Response001
	{
		std::uint32_t structSize = sizeof(Response001);
		Status status = Status::kInternalError;
		const char* jsonUtf8 = nullptr;
		std::uint32_t jsonBytes = 0;
	};

	struct Interface001
	{
		std::uint32_t structSize = sizeof(Interface001);
		std::uint32_t major = ServiceMajor;
		std::uint32_t minor = ServiceMinor;
		std::uint32_t schemaRevision = SchemaRevision;
		const void* context = nullptr;
		Status (*Dispatch)(const void*, const Request001*, Response001*) = nullptr;
	};
}
