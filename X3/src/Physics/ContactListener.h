#pragma once

#include "Physics/PhysicsTypes.h"
#include <Jolt/Physics/Collision/ContactListener.h>

namespace X3
{
	class PhysicsWorld;

	class PhysicsContactListener : public JPH::ContactListener
	{
	public:
		PhysicsContactListener(PhysicsWorld* world);

		// Called when a contact point is detected
		virtual JPH::ValidateResult OnContactValidate(
			const JPH::Body& inBody1,
			const JPH::Body& inBody2,
			JPH::RVec3Arg inBaseOffset,
			const JPH::CollideShapeResult& inCollisionResult) override;

		// Called when contact is added
		virtual void OnContactAdded(
			const JPH::Body& inBody1,
			const JPH::Body& inBody2,
			const JPH::ContactManifold& inManifold,
			JPH::ContactSettings& ioSettings) override;

		// Called when contact is persisted (still touching)
		virtual void OnContactPersisted(
			const JPH::Body& inBody1,
			const JPH::Body& inBody2,
			const JPH::ContactManifold& inManifold,
			JPH::ContactSettings& ioSettings) override;

		// Called when contact is removed
		virtual void OnContactRemoved(
			const JPH::SubShapeIDPair& inSubShapePair) override;

		// Get pending contact events for this frame
		const std::vector<ContactInfo>& GetContactsAdded() const { return m_ContactsAdded; }
		const std::vector<ContactInfo>& GetContactsPersisted() const { return m_ContactsPersisted; }
		const std::vector<std::pair<entt::entity, entt::entity>>& GetContactsRemoved() const { return m_ContactsRemoved; }

		// Clear contact lists (called at end of physics step)
		void ClearContacts();

	private:
		PhysicsWorld* m_World;

		// Contact events for this frame
		std::vector<ContactInfo> m_ContactsAdded;
		std::vector<ContactInfo> m_ContactsPersisted;
		std::vector<std::pair<entt::entity, entt::entity>> m_ContactsRemoved;

		std::mutex m_ContactMutex;
	};

	// Body activation listener for wake/sleep events
	class BodyActivationListener : public JPH::BodyActivationListener
	{
	public:
		BodyActivationListener(PhysicsWorld* world);

		virtual void OnBodyActivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData) override;
		virtual void OnBodyDeactivated(const JPH::BodyID& inBodyID, uint64_t inBodyUserData) override;

	private:
		PhysicsWorld* m_World;
	};
}
