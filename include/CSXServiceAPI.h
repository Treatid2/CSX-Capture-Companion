#pragma once

#include <cstdint>
#include <limits>

namespace CSX::ServiceAPI
{
	inline constexpr char ProviderName[] = "CommunityShaders";
	inline constexpr std::uint32_t RegistryMessageType = 0x43535852;  // "CSXR"
	inline constexpr std::uint32_t RegistryAbiMajor = 1;
	inline constexpr std::uint32_t RegistryAbiMinor = 0;

	enum class Status : std::uint32_t
	{
		kSuccess = 0,
		kInvalidArgument = 1,
		kStructureTooSmall = 2,
		kIncompatibleRegistryVersion = 3,
		kServiceNotFound = 4,
		kIncompatibleServiceVersion = 5,
		kMissingCapabilities = 6,
		kAlreadyRegistered = 7,
		kInternalError = 8
	};

	enum ServiceCapability : std::uint64_t
	{
		kCapabilityNone = 0,
		kCapabilityInspection = 1ull << 0,
		kCapabilityRuntimeMutation = 1ull << 1,
		kCapabilityPersistentMutation = 1ull << 2,
		kCapabilityDestructiveOperations = 1ull << 3,
		kCapabilityAsynchronousOperations = 1ull << 4,
		kCapabilityEventStream = 1ull << 5,
		kCapabilityTransactions = 1ull << 6
	};

	struct ServiceDescriptor001
	{
		std::uint32_t structSize = sizeof(ServiceDescriptor001);
		const char* name = nullptr;
		std::uint32_t major = 0;
		std::uint32_t minor = 0;
		std::uint32_t schemaRevision = 0;
		std::uint64_t capabilities = kCapabilityNone;
	};

	struct ServiceQuery001
	{
		std::uint32_t structSize = sizeof(ServiceQuery001);
		const char* name = nullptr;
		std::uint32_t major = 0;
		std::uint32_t minimumMinor = 0;
		std::uint32_t maximumMinor = std::numeric_limits<std::uint32_t>::max();
		std::uint64_t requiredCapabilities = kCapabilityNone;
	};

	struct ProducerIdentity001;
	struct Registry001
	{
		std::uint32_t structSize = sizeof(Registry001);
		std::uint32_t abiMajor = RegistryAbiMajor;
		std::uint32_t abiMinor = RegistryAbiMinor;
		const void* context = nullptr;
		Status (*GetProducerIdentity)(const void*, ProducerIdentity001*) = nullptr;
		std::uint32_t (*GetServiceCount)(const void*) = nullptr;
		Status (*GetServiceDescriptor)(const void*, std::uint32_t, ServiceDescriptor001*) = nullptr;
		Status (*QueryService)(const void*, const ServiceQuery001*, const void**, ServiceDescriptor001*) = nullptr;
	};

	struct RegistryMessage001
	{
		std::uint32_t structSize = sizeof(RegistryMessage001);
		std::uint32_t requestedAbiMajor = RegistryAbiMajor;
		std::uint32_t minimumAbiMinor = RegistryAbiMinor;
		Status status = Status::kInternalError;
		const Registry001* registry = nullptr;
	};
}
