#include "Physics/CollisionLayers.h"

namespace X3
{
	// ============================================================================
	// ObjectLayerPairFilter
	// ============================================================================

	bool ObjectLayerPairFilter::ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const
	{
		switch (inObject1)
		{
		case Layers::NON_MOVING:
			// Non-moving objects only collide with moving objects and characters
			return inObject2 == Layers::MOVING || inObject2 == Layers::CHARACTER;

		case Layers::MOVING:
			// Moving objects collide with everything except triggers (handled separately)
			return inObject2 != Layers::TRIGGER;

		case Layers::TRIGGER:
			// Triggers collide with moving objects and characters (for overlap detection)
			return inObject2 == Layers::MOVING || inObject2 == Layers::CHARACTER;

		case Layers::CHARACTER:
			// Characters collide with everything except other characters and triggers
			return inObject2 == Layers::NON_MOVING || inObject2 == Layers::MOVING;

		default:
			return false;
		}
	}

	// ============================================================================
	// BroadPhaseLayerInterface
	// ============================================================================

	BroadPhaseLayerInterface::BroadPhaseLayerInterface()
	{
		// Map object layers to broad phase layers
		m_ObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
		m_ObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
		m_ObjectToBroadPhase[Layers::TRIGGER] = BroadPhaseLayers::MOVING;
		m_ObjectToBroadPhase[Layers::CHARACTER] = BroadPhaseLayers::MOVING;
	}

	uint32_t BroadPhaseLayerInterface::GetNumBroadPhaseLayers() const
	{
		return BroadPhaseLayers::NUM_LAYERS;
	}

	JPH::BroadPhaseLayer BroadPhaseLayerInterface::GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const
	{
		JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
		return m_ObjectToBroadPhase[inLayer];
	}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char* BroadPhaseLayerInterface::GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const
	{
		switch ((JPH::BroadPhaseLayer::Type)inLayer)
		{
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
			return "NON_MOVING";
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
			return "MOVING";
		default:
			JPH_ASSERT(false);
			return "INVALID";
		}
	}
#endif

	// ============================================================================
	// ObjectVsBroadPhaseLayerFilter
	// ============================================================================

	bool ObjectVsBroadPhaseLayerFilter::ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const
	{
		switch (inLayer1)
		{
		case Layers::NON_MOVING:
			// Non-moving only collides with moving broad phase layer
			return inLayer2 == BroadPhaseLayers::MOVING;

		case Layers::MOVING:
		case Layers::TRIGGER:
		case Layers::CHARACTER:
			// Moving objects collide with both broad phase layers
			return true;

		default:
			JPH_ASSERT(false);
			return false;
		}
	}
}
