#pragma once

// =============================================================================
// MaterialDesc.h -- the AUTHORING-SIDE material.
//
// This is the CPU form: human-editable scalars plus texture references by GUID.
// Gpu::Material (Renderer/GpuTypes.h) is the runtime form: the same scalars
// packed into vec4s, with the GUIDs already resolved to indices in the bound
// material texture table. Renderer::Parse is the one place that converts, and it
// converts BOTH sources -- a MaterialComponent override and a mesh's imported
// material -- through the same path, so they cannot disagree.
//
// This deliberately does NOT live in Components.h: MeshMetadata carries the
// materials imported from a model file, and Project/Assets must not depend on
// Project/Scene.
//
// Phase 6 replaces the scalar set here with the layered OpenPBR reduction. The
// shape (authoring struct -> baked runtime struct) is what survives.
// =============================================================================

#include "lrpch.h"
#include "Core/GUID.h"

namespace X3
{

	struct MaterialDesc {
		glm::vec4 emission = { 0.0f, 0.0f, 0.0f, 1.0f };  // xyz colour, w strength
		glm::vec4 color    = { 1.0f, 1.0f, 1.0f, 1.0f };  // xyz base colour factor, w alpha

		float metallic    = 0.0f;
		float roughness   = 0.5f;
		float ao          = 1.0f;
		float normalScale = 1.0f;

		// LR_GUID::INVALID means "no texture, use the factor alone".
		LR_GUID baseColorTex  = LR_GUID::INVALID;
		LR_GUID normalTex     = LR_GUID::INVALID;
		LR_GUID metalRoughTex = LR_GUID::INVALID;
		LR_GUID emissiveTex   = LR_GUID::INVALID;
	};

}
