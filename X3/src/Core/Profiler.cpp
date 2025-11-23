#include "Profiler.h"

namespace Laura 
{	

	Profiler::ScopeTimer::ScopeTimer(Profiler* profiler, const std::string& label)
		: m_Profiler(profiler), m_Label(label) {
		m_Start = std::chrono::high_resolution_clock::now();
		m_Profiler->createTimerEntry(label);
	}

	Profiler::ScopeTimer::~ScopeTimer() {
		std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
		double elapsed_ms = std::chrono::duration<double, std::milli>(end - m_Start).count();
		m_Profiler->addTimerValue(m_Label, elapsed_ms);
	}

	Profiler::Profiler(const size_t capacityPerTimer) 
		: m_Capacity(capacityPerTimer){}

	const std::shared_ptr<Profiler::ScopeTimer> Profiler::globalTimer(const std::string& globalLabel) {
		m_GlobalLabel = globalLabel;
		globalTimerSet = true;
		return std::make_shared<Profiler::ScopeTimer>(this, m_GlobalLabel);
	}

	const std::shared_ptr<Profiler::ScopeTimer> Profiler::timer(const std::string& label) {
		assert(label != m_GlobalLabel); // use globalTimer() for timing the entire frame
		return std::make_shared<Profiler::ScopeTimer>(this, label);
	}

	void Profiler::createTimerEntry(const std::string& label){
		m_Data.emplace(label, ScrollingBuffer(m_Capacity));
	}

	void Profiler::addTimerValue(const std::string& label, const double elapsed_ms) {
		if (isPaused) {
			return;
		}

		auto it = m_Data.find(label);
		assert(it != m_Data.end()); // shouldn't ever happen
		it->second.push_back(elapsed_ms);
	};
}