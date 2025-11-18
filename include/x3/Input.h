#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <unordered_map>

namespace x3 {

struct ActionState {
    SDL_Scancode key;
    bool pressed = false;
    bool justPressed = false;
    bool justReleased = false;
};

class InputSystem {
public:
    void Init();
    void Update();
    
    void MapAction(const std::string& name, SDL_Scancode key);
    bool IsActionPressed(const std::string& name) const;
    bool IsActionJustPressed(const std::string& name) const;

private:
    std::unordered_map<std::string, ActionState> m_actions;
};

} // namespace x3
