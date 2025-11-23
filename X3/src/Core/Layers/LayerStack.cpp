#include "Core/Layers/LayerStack.h"

namespace Laura 
{

	LayerStack::~LayerStack() {
		for (std::shared_ptr<ILayer> layer : m_Layers) {
			layer->onDetach();
		}
	}

	void LayerStack::PushLayer(std::shared_ptr<ILayer> layer) {
		m_Layers.push_back(layer);
		layer->onAttach();
	}

	void LayerStack::PopLayer(std::shared_ptr<ILayer> layer) {
		m_Layers.erase(std::remove(m_Layers.begin(), m_Layers.end(), layer), m_Layers.end());
		layer->onDetach();
	}
	
	void LayerStack::onUpdate() {
		for (std::shared_ptr<ILayer> layer : m_Layers) {
			layer->onUpdate();
		}
	}

	void LayerStack::onDetach() {
		for (std::shared_ptr<ILayer> layer : m_Layers) {
			layer->onDetach();
		}
	}

	void LayerStack::dispatchEvent(std::shared_ptr<IEvent> event) {
		for (std::shared_ptr<ILayer> layer : m_Layers) {
			layer->onEvent(event);
			if (event->IsConsumed()) {
				break;
			}
		}
	}
}