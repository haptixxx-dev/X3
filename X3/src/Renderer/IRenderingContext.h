#pragma once

namespace Laura 
{

	class IRenderingContext {
	public:
		virtual void init() = 0;
		virtual void swapBuffers() = 0;
	};
}