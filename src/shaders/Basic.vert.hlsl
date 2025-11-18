struct VSInput {
    float3 Position : POSITION;
    // float3 Normal : NORMAL; // Uncomment when we have normals
    // float2 TexCoord : TEXCOORD0; // Uncomment when we have UVs
};

struct VSOutput {
    float4 Position : SV_Position;
    float4 Color : COLOR0;
};

cbuffer UniformBlock : register(b0) {
    float4x4 ViewProjection;
    float4x4 Model;
    float4 Color;
};

VSOutput main(VSInput input) {
    VSOutput output;
    float4 worldPos = mul(Model, float4(input.Position, 1.0));
    output.Position = mul(ViewProjection, worldPos);
    output.Color = Color;
    return output;
}
