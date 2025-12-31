# X3

C++ game engine

This engine is largely based on `Laura`, made by jakubg05.

## Editor Controls

### Camera Navigation
- **RMB + WASD/QE**: Free-fly camera movement (W/S forward/back, A/D left/right, Q/E down/up)
- **RMB + Shift**: 3x speed boost while moving
- **Shift + Scroll**: Adjust base movement speed (0.5 - 100 units/sec)
- **Alt + LMB**: Orbit around focus point
- **MMB Drag**: Pan camera
- **Scroll**: Zoom in/out

### Gizmo Controls
- **G**: Translate mode
- **R**: Rotate mode
- **S**: Scale mode (only when RMB not held)
- Gizmo shortcuts only activate when viewport is hovered and RMB is not pressed

## Development Roadmap

### P0 - High Priority

- [x] Fix editor camera system
  - [x] Fix camera rotation matrix (proper YX order)
  - [x] Fix movement relative to camera direction
  - [x] Fix Alt+LMB orbit mode
  - [x] Add speed adjustment (Shift+Scroll)
  - [x] Fix gizmo alignment with rendered scene
  - [x] Fix key conflicts between gizmo shortcuts and movement
- [ ] Integrate Jolt Physics engine
  - [ ] Add Jolt as dependency
  - [ ] Create RigidBodyComponent
  - [ ] Create ColliderComponent (box, sphere, capsule)
  - [ ] Implement physics update loop in Scene
  - [ ] Add physics debug rendering
- [ ] Add runtime camera controller
  - [ ] First-person camera component
  - [ ] Third-person camera component
- [ ] Implement scripting system (Squirrel)
  - [ ] Integrate Squirrel scripting language
  - [ ] Add ScriptComponent
  - [ ] Script lifecycle (OnStart, OnUpdate, OnDestroy)
  - [ ] Bind engine API to Squirrel (entities, transforms, input, etc.)
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
- [ ] Gaussian splat rendering
  - [ ] .ply splat file loading
  - [ ] GPU splat sorting & rasterization
  - [ ] Depth compositing with mesh rendering
  - [ ] GaussianSplatComponent (for non-collision decorative objects)
  - [ ] Runtime splat manipulation API (position, color, opacity)
- [ ] Advanced rendering features
  - [ ] Post-processing stack
  - [ ] Volumetric effects
- [ ] Scene serialization improvements
  - [ ] Binary format for faster loading
  - [ ] Scene streaming/LOD
