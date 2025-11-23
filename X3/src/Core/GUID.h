#pragma once

#include "lrpch.h"

namespace Laura
{

	// Globally Unique IDentifier
	// A wrapper for a uint64_t
	class LR_GUID {
	public:
		static const LR_GUID INVALID;

		LR_GUID(); // constructs a random uint64_t GUID
		LR_GUID(uint64_t guid);
		LR_GUID(const LR_GUID& other); // copy constructor
		~LR_GUID() = default;
		
		bool operator==(const LR_GUID& other) const { return m_GUID == other.m_GUID; }
		bool operator!=(const LR_GUID& other) const { return m_GUID != other.m_GUID; }
		operator uint64_t() const { return m_GUID; } // uint64_t conversion operator 

		inline std::string string() const {
			return std::to_string(m_GUID);
		}
		
	private:
		uint64_t m_GUID;
	};
}


// Hashing specialization for GUIDs for use in std::unordered_map ...
namespace std
{
	template<>
	struct hash<Laura::LR_GUID>
	{
		std::size_t operator()(const Laura::LR_GUID& guid) const
		{
			return std::hash<uint64_t>()((uint64_t)guid);
		}
	};
}