#!/usr/bin/env python3
"""Generate the C++ descriptor set layout table from Slang reflection JSON.

WHY THIS EXISTS. Before Phase 3, X3/src/Renderer/Renderer.cpp carried a
hand-written `kComputeSetLayouts` matched to the shader source by comment. Any
new binding, any reordering, any change of descriptor type had to be mirrored by
hand into C++, and getting it wrong produced either a validation error at
dispatch or -- worse -- a silently mismatched binding that read the wrong
resource. `DescriptorWriter::flush()`'s completeness assert could only check
that the table was internally consistent, not that it matched the shader.

Now slangc emits reflection for each entry point, this script turns that into
the table, and the C++ cannot disagree with the shader because it is derived
from it.

There is NO OFF-THE-SHELF TOOL for this. Slang's reflection API and its JSON
dump are well supported and its own docs describe the pattern, but the
JSON -> C++ descriptor table step is something every project writes itself.
ENGINE_PLAN.md budgeted for that; this is it.

Usage:
    gen_descriptor_tables.py --out <header> <reflection.json> [<reflection.json> ...]

All inputs must agree on the layout. Every compute entry point in this engine
declares the same three sets from the same Bindings.slang module, so a
disagreement means one of them imported something different -- which is an error
worth failing the build over, not something to merge.
"""

import argparse
import json
import os
import sys

# Slang reflection type -> the VkDescriptorType the resource needs.
#
# Keyed on (kind, baseShape, access, combined) as they appear in the JSON. The
# cases here are exactly the ones this engine's shaders use; an unrecognised
# combination is a hard error rather than a guess, because guessing produces a
# table that compiles and binds the wrong thing.
def vk_descriptor_type(t):
    kind = t.get("kind")

    if kind == "constantBuffer":
        return "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"

    if kind == "resource":
        shape = t.get("baseShape")
        access = t.get("access", "read")
        combined = t.get("combined", False)

        if shape == "structuredBuffer":
            return "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER"
        if shape == "texture2D":
            if access == "readWrite":
                return "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE"
            if combined:
                return "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER"
            return "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE"

    return None


def resolve(t):
    """Peel an array wrapper, returning (elementType, count)."""
    if t.get("kind") == "array":
        return t.get("elementType", {}), int(t.get("elementCount", 1))
    return t, 1


def collect(path):
    """(set, binding) -> (name, vkType, count) for one reflection file."""
    with open(path) as f:
        doc = json.load(f)

    out = {}
    for param in doc.get("parameters", []):
        name = param.get("name", "<unnamed>")
        binding = param.get("binding", {})

        # Only descriptor-table-backed parameters produce set/binding pairs.
        # Push constants and specialization constants report other kinds and
        # are not part of a descriptor set layout.
        if binding.get("kind") != "descriptorTableSlot":
            continue

        space = int(binding.get("space", 0))
        index = int(binding.get("index", 0))

        elem, count = resolve(param.get("type", {}))
        vk_type = vk_descriptor_type(elem)
        if vk_type is None:
            sys.exit(
                f"{path}: cannot map parameter '{name}' "
                f"(kind={elem.get('kind')}, shape={elem.get('baseShape')}, "
                f"access={elem.get('access')}, combined={elem.get('combined')}) "
                f"to a VkDescriptorType. Teach vk_descriptor_type() about it "
                f"rather than letting the table guess."
            )

        out[(space, index)] = (name, vk_type, count)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("reflection", nargs="+")
    args = ap.parse_args()

    merged = None
    origin = None
    for path in args.reflection:
        table = collect(path)
        if merged is None:
            merged, origin = table, path
            continue
        if table != merged:
            only_a = sorted(set(merged) - set(table))
            only_b = sorted(set(table) - set(merged))
            differing = sorted(k for k in set(merged) & set(table) if merged[k] != table[k])
            sys.exit(
                f"descriptor layout mismatch between {origin} and {path}.\n"
                f"  only in {origin}: {only_a}\n"
                f"  only in {path}: {only_b}\n"
                f"  differing: "
                + "; ".join(f"{k}: {merged[k]} vs {table[k]}" for k in differing)
                + "\nEvery compute entry point must declare the same sets, which "
                "they do by importing Bindings.slang. One of them diverged."
            )

    if not merged:
        sys.exit("no descriptor bindings found in any reflection file")

    set_count = max(space for space, _ in merged) + 1
    by_set = {s: [] for s in range(set_count)}
    for (space, index), value in sorted(merged.items()):
        by_set[space].append((index, value))

    lines = []
    w = lines.append
    w("// GENERATED FILE -- DO NOT EDIT.")
    w("//")
    w("// Produced by scripts/gen_descriptor_tables.py from the Slang reflection")
    w("// output of X3/res/shaders/*.slang. Edit Bindings.slang and rebuild; the")
    w("// build regenerates this and a stale copy cannot survive a compile.")
    w("//")
    w("// This replaces the hand-written table that used to live in Renderer.cpp,")
    w("// matched to the shader source by comment. The C++ can no longer disagree")
    w("// with the shader, because it is derived from it.")
    w("#pragma once")
    w("")
    w('#include "Platform/Vulkan/VulkanTypes.h"')
    w("")
    w("#include <vector>")
    w("")
    w("namespace X3::Generated")
    w("{")
    w("")
    w("\t// Set N is entry N. Every compute pipeline in this engine uses this same")
    w("\t// table, because every entry point imports the same Bindings.slang.")
    w("\tinline const std::vector<std::vector<DescriptorBindingDesc>> kComputeSetLayouts = {")

    for space in range(set_count):
        entries = by_set[space]
        w(f"\t\t// ---- set {space} ----")
        w("\t\t{")
        for index, (name, vk_type, count) in entries:
            w(f"\t\t\t{{{index}, {vk_type}, {count}, VK_SHADER_STAGE_COMPUTE_BIT}},"
              f"   // {name}")
        w("\t\t},")

    w("\t};")
    w("")

    # Surface the material table size so the C++ constant can be checked against
    # the shader's rather than merely matching it by convention.
    for (space, index), (name, vk_type, count) in sorted(merged.items()):
        if count > 1:
            w(f"\t// Array binding declared by the shader, for cross-checking against")
            w(f"\t// the C++ constant that fills it.")
            w(f"\tinline constexpr uint32_t k{name[0].upper()}{name[1:]}Count = {count}u;")
            w("")

    w("}")
    w("")

    text = "\n".join(lines)

    # Only rewrite when the content actually changed, so an unchanged shader
    # does not retrigger a full rebuild of everything that includes this.
    if os.path.exists(args.out):
        with open(args.out) as f:
            if f.read() == text:
                return

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
