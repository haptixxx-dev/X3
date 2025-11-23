#include "BVHAccel.h"
#include <algorithm> // std::min

namespace Laura
{

	BVHAccel::BVHAccel(const std::vector<Triangle>& meshBuffer, const uint32_t firstTriIdx, const uint32_t triCount) 
	: m_TriBuff(meshBuffer), m_FirstTriIdx(firstTriIdx), m_TriCount(triCount) {
		m_Centroids = PrecomputeCentroids();
	}

	void BVHAccel::Build(std::vector<Node>& nodeBuffer, std::vector<uint32_t>& indexBuffer, uint32_t& firstNodeIdx, uint32_t& nodeCount) {
		const size_t N = m_TriCount; // for convenience

		size_t oldSize = nodeBuffer.size();

		firstNodeIdx = oldSize;
		// firstIndexBuffIdx == m_FirstTriIdx;
		
		// make space for new data
		nodeBuffer.resize(nodeBuffer.size() + 2 * N - 1);
		indexBuffer.resize(indexBuffer.size() + N);

		m_NodeBuff = &nodeBuffer[firstNodeIdx];
		m_IdxBuff = &indexBuffer[m_FirstTriIdx];
		for (int i = 0; i < N; i++) {
			m_IdxBuff[i] = i;
		}

		m_NodesUsed = 0;
		Node& root = m_NodeBuff[m_NodesUsed++];
		root.leftChild_Or_FirstTri = 0;
		root.triCount = N;

		UpdateAABB(root);
		SubDivide(root);

		nodeCount = m_NodesUsed;
		nodeBuffer.resize(oldSize + m_NodesUsed); // Erase the unused space ( if we used less than 2N-1 nodes)
	}

	void BVHAccel::UpdateAABB(Node& node) {
		Aabb aabb;
		// iterate over primitives contained by the Node
		for (size_t i = 0; i < node.triCount; i++) { // every 3rd vertex is new triangle
			const Triangle& t = m_TriBuff[m_FirstTriIdx + m_IdxBuff[node.leftChild_Or_FirstTri + i]];
			aabb.grow(glm::vec3(t.v0));
			aabb.grow(glm::vec3(t.v1));
			aabb.grow(glm::vec3(t.v2));
		}
		node.min = aabb.boxMin;
		node.max = aabb.boxMax;
	}

	// SAH Heuristic in O(n) time
	float BVHAccel::FindBestSplitPlane(Node& node, int& splitAxis, float& splitPos) {
		const size_t BINS = 8;
		float bestPos = 0, bestCost = FLT_MAX;
		for (int axis = 0; axis < 3; axis++) {
			// finding the bounds of the triangle centroids
			float aabbMin = FLT_MAX;
			float aabbMax = -FLT_MAX;
			for (int i = 0; i < node.triCount; i++) {
				glm::vec3& centroid = m_Centroids[m_IdxBuff[node.leftChild_Or_FirstTri + i]];
				aabbMin = glm::min(aabbMin, centroid[axis]);
				aabbMax = glm::max(aabbMax, centroid[axis]);
			}

			if (aabbMin == aabbMax) { 
				continue; 
			}

			// splitting the bounds into bins (allows merging intervals quickly)
			Bin bin[BINS];
			float scale = BINS / (aabbMax - aabbMin);
			for (uint32_t i = 0; i < node.triCount; i++) {
				size_t triIdx = m_IdxBuff[node.leftChild_Or_FirstTri + i];
				const Triangle& tri = m_TriBuff[m_FirstTriIdx + triIdx];
				size_t binIdx = std::min(BINS - 1, (size_t)((m_Centroids[triIdx][axis] - aabbMin) * scale));
				bin[binIdx].triCount++;
				bin[binIdx].aabb.grow(tri.v0);
				bin[binIdx].aabb.grow(tri.v1);
				bin[binIdx].aabb.grow(tri.v2);
			}

			float leftArea[BINS - 1], rightArea[BINS - 1];
			int leftCount[BINS - 1], rightCount[BINS - 1];
			// both directions simultaneously 
			size_t currLeftCount = 0, currRightCount = 0;
			Aabb currLeftAabb, currRightAabb;
			for (uint32_t i = 0; i < BINS-1; i++) {
				currLeftCount += bin[i].triCount;
				currLeftAabb.grow(bin[i].aabb);
				leftArea[i] = currLeftAabb.area();
				leftCount[i] = currLeftCount;

				currRightCount += bin[BINS - 1 - i].triCount;
				currRightAabb.grow(bin[BINS - 1 - i].aabb);
				// -2 because left/rightArea have size = BINS - 1
				rightArea[BINS - 2 - i] = currRightAabb.area();
				rightCount[BINS - 2 - i] = currRightCount;
			}

			scale = (aabbMax - aabbMin) / BINS;
			for (int i = 0; i < BINS - 1; i++) {
				float cost = leftCount[i] * leftArea[i] + rightCount[i] * rightArea[i];
				if (cost < bestCost) {
					splitAxis = axis;
					splitPos = aabbMin + scale * (i + 1);
					bestCost = cost;
				}
			}
		}
		return bestCost;
	}

	void BVHAccel::SubDivide(Node& node) {
		int bestAxis = -1;
		float bestPos = 0;
		float bestCost = FindBestSplitPlane(node, bestAxis, bestPos);

		Aabb parentAabb{ node.min, node.max };
		float parentCost = node.triCount * parentAabb.area();
		if (bestCost >= parentCost) {
			return;
		}

		uint32_t leftPtr = node.leftChild_Or_FirstTri; // points to the firstTri in node's triangles
		uint32_t rightPtr = node.leftChild_Or_FirstTri + node.triCount - 1; // points to the lastTri

		// partition/sort the triangles (quicksort partition)
		while (leftPtr <= rightPtr) {
			if (m_Centroids[m_IdxBuff[leftPtr]][bestAxis] < bestPos) {
				leftPtr++;
			}
			else {
				Swap(leftPtr, rightPtr--); // swap and decrement right
			}
		}

		int leftTriCount = leftPtr - node.leftChild_Or_FirstTri; // distance between firstTri and partition point
		
		// couldn't partition
		if (leftTriCount == 0 || leftTriCount == node.triCount) {
			return;
		}

		// find indices for the new child nodes
		int leftChildIdx = m_NodesUsed++;
		int rightChildIdx = m_NodesUsed++;

		// populate children
		m_NodeBuff[leftChildIdx].leftChild_Or_FirstTri = node.leftChild_Or_FirstTri;
		m_NodeBuff[leftChildIdx].triCount = leftTriCount;
		m_NodeBuff[rightChildIdx].leftChild_Or_FirstTri = node.leftChild_Or_FirstTri + leftTriCount;
		m_NodeBuff[rightChildIdx].triCount = node.triCount - leftTriCount;
		node.triCount = 0; // ! mark the node as non-leaf node
		node.leftChild_Or_FirstTri = leftChildIdx; // now points to the leftChild node

		UpdateAABB(m_NodeBuff[leftChildIdx]); // figure out bounds based on the recently added triangle indices and counts
		UpdateAABB(m_NodeBuff[rightChildIdx]);
		
		SubDivide(m_NodeBuff[leftChildIdx]);
		SubDivide(m_NodeBuff[rightChildIdx]);
	}
}