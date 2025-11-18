#include "x3/Input.h"

namespace x3 {

void InputSystem::Init() {
    // SDL_GetKeyboardState is available after SDL_Init
}

void InputSystem::Update() {
    int numKeys;
    const bool* state = SDL_GetKeyboardState(&numKeys);

    for (auto& [name, action] : m_actions) {
        if (action.key >= numKeys) continue; // Safety check

        bool currentlyPressed = state[action.key];
        
        action.justPressed = currentlyPressed && !action.pressed;
        action.justReleased = !currentlyPressed && action.pressed;
        action.pressed = currentlyPressed;
    }
}

void InputSystem::MapAction(const std::string& name, SDL_Scancode key) {
    ActionState action;
    action.key = key;
    m_actions[name] = action;
}

bool InputSystem::IsActionPressed(const std::string& name) const {
    auto it = m_actions.find(name);
    if (it != m_actions.end()) {
        return it->second.pressed;
    }
    return false;
}

bool InputSystem::IsActionJustPressed(const std::string& name) const {
    auto it = m_actions.find(name);
    if (it != m_actions.end()) {
        return it->second.justPressed;
    }
    return false;
}

} // namespace x3
