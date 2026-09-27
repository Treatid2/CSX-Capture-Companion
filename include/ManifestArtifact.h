#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace CSXCaptureCompanion
{
	/// Receipt-bound custody for a committed Screenshot API manifest.
	struct ManifestArtifact
	{
		std::filesystem::path path;
		std::uint64_t bytes{};
		std::string sha256;

		[[nodiscard]] bool Empty() const noexcept { return path.empty(); }
	};
}
