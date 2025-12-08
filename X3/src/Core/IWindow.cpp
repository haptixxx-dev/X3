#include "Core/IWindow.h"
#include "Platform/Windows/GLFWWindow.h"

namespace X3
{

	std::shared_ptr<IWindow> IWindow::createWindow(WindowProps windowProps = WindowProps{}) {
		return std::make_shared<GLFWWindowIMPL>(windowProps);
	}
}