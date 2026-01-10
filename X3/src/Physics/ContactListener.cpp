#include "Physics/ContactListener.h"
#include "Physics/PhysicsWorld.h"

namespace X3
{
	// ============================================================================
	// PhysicsContactListener
	// ============================================================================

	PhysicsContactListener::PhysicsContactListener(PhysicsWorld* world)
		: m_World(world)
	{
	}

	JPH::ValidateResult PhysicsContactListener::OnContactValidate(
		const JPH::Body& inBody1,
		const JPH::Body& inBody2,
		JPH::RVec3Arg inBaseOffset,
		const JPH::CollideShapeResult& inCollisionResult)
	{
		// Accept all contacts by default
		// Could be used to filter contacts based on game logic
		return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
	}

	void PhysicsContactListener::OnContactAdded(
		const JPH::Body& inBody1,
		const JPH::Body& inBody2,
		const JPH::ContactManifold& inManifold,
		JPH::ContactSettings& ioSettings)
	{
		std::lock_guard<std::mutex> lock(m_ContactMutex);

		ContactInfo info;
		info.entityA = m_World->GetEntityFromBodyID(inBody1.GetID());
		info.entityB = m_World->GetEntityFromBodyID(inBody2.GetID());

		// Use the first contact point
		if (inManifold.mRelativeContactPointsOn1.size() > 0) {
			JPH::Vec3 contactPoint = inManifold.GetWorldSpaceContactPointOn1(0);
			info.contactPoint = FromJolt(contactPoint);
		}

		info.contactNormal = FromJolt(inManifold.mWorldSpaceNormal);
		info.penetrationDepth = inManifold.mPenetrationDepth;

		m_ContactsAdded.push_back(info);
	}

	void PhysicsContactListener::OnContactPersisted(
		const JPH::Body& inBody1,
		const JPH::Body& inBody2,
		const JPH::ContactManifold& inManifold,
		JPH::ContactSettings& ioSettings)
	{
		std::lock_guard<std::mutex> lock(m_ContactMutex);

		ContactInfo info;
		info.entityA = m_World->GetEntityFromBodyID(inBody1.GetID());
		info.entityB = m_World->GetEntityFromBodyID(inBody2.GetID());

		if (inManifold.mRelativeContactPointsOn1.size() > 0) {
			JPH::Vec3 contactPoint = inManifold.GetWorldSpaceContactPointOn1(0);
			info.contactPoint = FromJolt(contactPoint);
		}

		info.contactNormal = FromJolt(inManifold.mWorldSpaceNormal);
		info.penetrationDepth = inManifold.mPenetrationDepth;

		m_ContactsPersisted.push_back(info);
	}

	void PhysicsContactListener::OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair)
	{
		std::lock_guard<std::mutex> lock(m_ContactMutex);

		entt::entity entityA = m_World->GetEntityFromBodyID(inSubShapePair.GetBody1ID());
		entt::entity entityB = m_World->GetEntityFromBodyID(inSubShapePair.GetBody2ID());

		m_ContactsRemoved.push_back({ entityA, entityB });
	}

	void PhysicsContactListener::ClearContacts()
	{
		std::lock_guard<std::mutex> lock(m_ContactMutex);
		m_ContactsAdded.clear();
		m_ContactsPersisted.clear();
		m_ContactsRemoved.clear();
	}

	// ============================================================================
	// BodyActivationListener
	// ============================================================================

	BodyActivationListener::BodyActivationListener(PhysicsWorld* world)
		: m_World(world)
	{
	}

	void BodyActivationListener::OnBodyActivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData)
	{
		// Body woke up - could dispatch event
		LOG_ENGINE_TRACE("Physics body activated: {}", inBodyID.GetIndex());
	}

	void BodyActivationListener::OnBodyDeactivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData)
	{
		// Body went to sleep - could dispatch event
		LOG_ENGINE_TRACE("Physics body deactivated: {}", inBodyID.GetIndex());
	}
}
