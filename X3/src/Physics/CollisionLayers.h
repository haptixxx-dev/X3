#pragma once

#include "Physics/PhysicsTypes.h"

namespace X3
{
	// Collision object layers - determines which objects can collide
	namespace Layers
	{
		static constexpr JPH::ObjectLayer NON_MOVING = 0;  // Static world geometry
		static constexpr JPH::ObjectLayer MOVING = 1;      // Dynamic/kinematic objects
		static constexpr JPH::ObjectLayer TRIGGER = 2;     // Trigger volumes (no collision response)
		static constexpr JPH::ObjectLayer CHARACTER = 3;   // Character controllers
		static constexpr JPH::ObjectLayer NUM_LAYERS = 4;
	}

	// Broad phase layers - coarse collision filtering
	namespace BroadPhaseLayers
	{
		static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
		static constexpr JPH::BroadPhaseLayer MOVING(1);
		static constexpr uint32_t NUM_LAYERS = 2;
	}

	// Maps object layers to broad phase layers
	class ObjectLayerPairFilter : public JPH::ObjectLayerPairFilter
	{
	public:
		virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override;
	};

	// Maps object layers to broad phase layers
	class BroadPhaseLayerInterface : public JPH::BroadPhaseLayerInterface
	{
	public:
		BroadPhaseLayerInterface();

		virtual uint32_t GetNumBroadPhaseLayers() const override;
		virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override;

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override;
#endif

	private:
		JPH::BroadPhaseLayer m_ObjectToBroadPhase[Layers::NUM_LAYERS];
	};

	// Determines if objects in broad phase layers can collide
	class ObjectVsBroadPhaseLayerFilter : public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override;
	};
}
