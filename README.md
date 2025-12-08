# X3

C++ game engine

This engine is largely based on `Laura`, made by jakubg05.

## Development Roadmap

### P0 - High Priority
- [x] Fix camera X rotation (inverted pitch)
- [ ] Integrate Jolt Physics engine
  - [ ] Add Jolt as dependency
  - [ ] Create RigidBodyComponent
  - [ ] Create ColliderComponent (box, sphere, capsule)
  - [ ] Implement physics update loop in Scene
  - [ ] Add physics debug rendering
- [ ] Add runtime camera controller
  - [ ] First-person camera component
  - [ ] Third-person camera component
- [ ] Implement scripting system
  - [ ] Evaluate Lua vs C# scripting
  - [ ] Add ScriptComponent
  - [ ] Script lifecycle (OnStart, OnUpdate, OnDestroy)
  - [ ] Script hot-reloading

### P1 - Medium-High Priority
- [ ] Audio system
  - [ ] AudioSourceComponent (3D spatial audio)
  - [ ] AudioListenerComponent
  - [ ] SDL_mixer integration
  - [ ] Support wav, mp3, ogg formats
- [ ] Prefab system
  - [ ] Save entities as prefabs
  - [ ] Prefab instantiation
  - [ ] Prefab inheritance/variants
- [ ] Input system improvements
  - [ ] Input mapping/rebinding
  - [ ] Gamepad support
  - [ ] Input action system

### P2 - Medium Priority
- [ ] Animation system
  - [ ] Skeletal animation support
  - [ ] Animation blending
  - [ ] Animation state machine
- [ ] Particle system
  - [ ] ParticleEmitterComponent
  - [ ] GPU compute-based particles
  - [ ] Particle collision with physics
- [ ] Gameplay components
  - [ ] HealthComponent
  - [ ] InventoryComponent
  - [ ] TriggerComponent (collision events)
  - [ ] AIComponent (basic behavior trees)

### P3 - Low Priority
- [ ] UI system (runtime, not editor)
  - [ ] Canvas/widget system
  - [ ] UI interaction with physics raycasts
- [ ] Networking
  - [ ] Client-server architecture
  - [ ] Entity replication
- [ ] Advanced rendering features
  - [ ] Post-processing stack
  - [ ] Volumetric effects
- [ ] Scene serialization improvements
  - [ ] Binary format for faster loading
  - [ ] Scene streaming/LOD
