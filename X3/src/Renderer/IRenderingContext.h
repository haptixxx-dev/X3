#pragma once

namespace X3 
{

	class IRenderingContext {
	public:
		virtual void init() = 0;
		virtual void swapBuffers() = 0;
	};
}