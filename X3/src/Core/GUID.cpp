#include "GUID.h"
#include <random>
#include <cstdint>

namespace Laura
{

	// global for this translation unit (static)
	static std::random_device s_RandomDevice;
	static std::mt19937_64 s_RandomEngine(s_RandomDevice());
	static std::uniform_int_distribution<uint64_t> s_UniformDistribution;
	const LR_GUID LR_GUID::INVALID = LR_GUID(0);

	LR_GUID::LR_GUID()
		: m_GUID(s_UniformDistribution(s_RandomEngine)) {
	}

	LR_GUID::LR_GUID(uint64_t guid)
		: m_GUID(guid) {
	}

	LR_GUID::LR_GUID(const LR_GUID& other)
		: m_GUID(other.m_GUID) {
	}
}
