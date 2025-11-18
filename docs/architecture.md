# Architecture

X3 follows a strict separation between the engine core and game logic, mediated by the ECS.

## Core Systems

### Window & Renderer
The engine wraps SDL3's `SDL_Window` and `SDL_Renderer`. The renderer is currently focused on 2D primitives but is designed to be replaced or extended.

### Entity Component System (ECS)
We use [Flecs](https://github.com/SanderMertens/flecs) as our ECS backend.

- **Entities**: Game objects (Player, Enemy, Bullet).
- **Components**: Data structs (Position, Velocity, Sprite).
- **Systems**: Logic that processes entities with specific components (MoveSystem, RenderSystem).

## Directory Structure

- `src/engine/`: Core engine source code.
- `src/game/`: Game-specific logic (example).
- `include/x3/`: Public API headers.
- `docs/`: Documentation.
