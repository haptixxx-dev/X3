#pragma once

#include "lrpch.h"

namespace Laura
{

	class ScrollingBuffer {
	public:
		ScrollingBuffer(size_t capacity) : m_Capacity(capacity) {
			m_Data.resize(2 * m_Capacity);
		}
		~ScrollingBuffer() = default;

		inline static const int mod(int a, int m) {
			return ((a % m) + m) % m; // True Mathematical modulo  -1 % 10 = 9
		}

		inline void push_back(double val){
			assert(m_Capacity != 0);

			m_Data[m_WriteIdx] = val;
			m_Data[m_WriteIdx + m_Capacity] = val;
			m_WriteIdx = (m_WriteIdx + 1) % m_Capacity;
			if (m_Size < m_Capacity) m_Size++;
		}

		inline const double* data(int span = 100) const {
			assert(m_Capacity != 0);
			// Return a pointer to a contiguous block of "reduced" data.
			if (m_Size < m_Capacity)
				return &m_Data[0 + (m_Size - spanSize(span))];
			return &m_Data[m_WriteIdx + (m_Size - spanSize(span))];
		}

		inline void clear() {
			m_Size = 0;
			m_WriteIdx = 0;
		}

		inline const size_t size(int span = 100) const { return spanSize(span); }
		inline const bool empty() const { return m_Size == 0; }
		
		inline const double average(int span = 100) const{
			int rSize = spanSize(span);
			if (rSize == 0) 
				return 0;

			double sum = 0;
			int startingIdx = mod(m_WriteIdx - rSize, m_Capacity);
			for (size_t i = 0; i < rSize; i++) {
				sum += m_Data[(startingIdx + i) % m_Capacity];
			}
			return sum / rSize;
		}

	private:
		inline const size_t spanSize(int span) const {
			if (span > 100) span = 100;
			if (span < 1) span = 1;
			return std::ceil((double)m_Size * ((double)span / 100));
		}

		std::vector<double> m_Data;
		const size_t m_Capacity = 0;
		int m_WriteIdx = 0;
		size_t m_Size = 0;
	};



	class Profiler {
	public:
		class ScopeTimer {
		public:
			ScopeTimer(Profiler* profiler, const std::string& label);
			~ScopeTimer();
		private:
			Profiler* m_Profiler;
			std::string m_Label;
			std::chrono::high_resolution_clock::time_point m_Start;
		};

		Profiler(const size_t capacityPerTimer);
		~Profiler() = default;

		// Timing
		const std::shared_ptr<ScopeTimer> globalTimer(const std::string& globalLabel);
		const std::shared_ptr<ScopeTimer> timer(const std::string& label);

		// ScopeTimer used Only methods
		void createTimerEntry(const std::string& label);
		void addTimerValue(const std::string& label, const double elapsed_ms);

		// Getters, Setters & Utility
		inline const ScrollingBuffer& getGlobalBuffer() const {
			assert(globalTimerSet); // first set the globalTimer() before calling getGlobalBuffer()
			return m_Data.find(m_GlobalLabel)->second;
		}

		inline void clear() {
			for (auto& it : m_Data) {
				it.second.clear();
			}
		}

		inline const std::unordered_map<std::string, ScrollingBuffer> data() const { return m_Data; }
		inline const size_t capacity() const { return m_Capacity; }
		inline const std::string getGlobalLabel() const { return m_GlobalLabel; }
		
		bool globalTimerSet = false;
		bool isPaused = false;
	private:
		std::string m_GlobalLabel = "GLOBAL"; // default
		std::unordered_map<std::string, ScrollingBuffer> m_Data;
		const size_t m_Capacity;
	};
}