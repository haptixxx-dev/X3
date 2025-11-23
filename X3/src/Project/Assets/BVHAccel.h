// Thanks to: https://jacco.ompf2.com/2022/04/13/how-to-build-a-bvh-part-1-basics
#pragma once

#include "lrpch.h"
#include "Project/Assets/AssetTypes.h"

namespace Laura
{

	class BVHAccel {
	public:
		// according to std430 - 32 bytes (allows packing of vec3, uint into 16 bytes)
		struct Node {
			glm::vec3 min;
			uint32_t leftChild_Or_FirstTri;
			glm::vec3 max;
			uint32_t triCount;
			/*	if primCount == 0: leftChild_Or_FirstTri == leftChild
				else leftChild_Or_FirstTri == firstTri */
		};

		struct Aabb {
			glm::vec3 boxMin;
			glm::vec3 boxMax;

			Aabb(glm::vec3 boxMin = glm::vec3(FLT_MAX), glm::vec3 boxMax = glm::vec3(-FLT_MAX))
			: boxMin(boxMin), boxMax(boxMax) {}

			void grow(glm::vec3 p) {
				boxMin = glm::min(boxMin, p); 
				boxMax = glm::max(boxMax, p);
			}

			void grow(const Aabb& aabb) {
				boxMin = glm::min(boxMin, aabb.boxMin);
				boxMax = glm::max(boxMax, aabb.boxMax);
			}

			float area() { 
				glm::vec3 s = boxMax - boxMin; // box size
				return s.x * s.y + s.y * s.z + s.z * s.x; // no * 2 as constants don't matter
			}
		};

		struct Bin {
			Aabb aabb;
			int triCount = 0;
		};

		BVHAccel(const std::vector<Triangle>& meshBuffer, const uint32_t firstTriIdx, const uint32_t triCount);
		~BVHAccel() = default;

		// Builds the Bounding Volume Hierarchy for a given Mesh using the UpdateAABB() & SubDivide() helper methods
		void Build(std::vector<Node>& nodeBuffer, std::vector<uint32_t>& indexBuffer, uint32_t& firstNodeIdx, uint32_t& nodeCount);

	private:
		float FindBestSplitPlane(Node& node, int& axis, float& splitPos);

		float EvaluateSAH(Node& node, int axis, float candidatePos);
		// Computes the Axis Aligned Bounding Box for a Node passed in using its triangles
		void UpdateAABB(Node& node);
		// Recursively splits the node using a split method, and sorts the triangle index array
		void SubDivide(Node& node);

		inline const std::vector<glm::vec3> PrecomputeCentroids() const {
			std::vector<glm::vec3> centroids;
			centroids.resize(m_TriCount);
			for (int i = 0; i < m_TriCount; i++) {
				const Triangle& t = m_TriBuff[m_FirstTriIdx + i];
				centroids[i] = (t.v0 + t.v1 + t.v2) * 0.333333333333f;
			}
			return centroids;
		}

		inline void Swap(int idx1, int idx2) {
			uint32_t tmp = m_IdxBuff[idx1];
			m_IdxBuff[idx1] = m_IdxBuff[idx2];
			m_IdxBuff[idx2] = tmp;
		}
		
		// passed into the constructor
		const std::vector<Triangle>& m_TriBuff;
		const uint32_t m_FirstTriIdx;
		const uint32_t m_TriCount;

		std::vector<glm::vec3> m_Centroids;

		uint32_t m_NodesUsed = 0;
		Node* m_NodeBuff = nullptr;
		uint32_t* m_IdxBuff = nullptr;
	};
}