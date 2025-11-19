struct VSInput {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    // float2 TexCoord : TEXCOORD0; // Uncomment when we have UVs
};

struct VSOutput {
    float4 Position : SV_Position;
    float3 WorldPos : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Color : COLOR0;
};

cbuffer UniformBlock : register(b0) {
    float4x4 ViewProjection;
    float4x4 Model;
    float4 Color;
    float4 LightPosition;
    float4 ViewPosition;
    float4 AmbientColor;
    float4 LightColor;
    float SpecularPower;
    float SpecularStrength;
    float2 _pad;
};

VSOutput main(VSInput input) {
    VSOutput output;
    float4 worldPos = mul(Model, float4(input.Position, 1.0));
    output.Position = mul(ViewProjection, worldPos);
    output.Normal = normalize(mul((float3x3)Model, input.Normal));
    output.WorldPos = worldPos.xyz;
    output.Color = Color;
    return output;
}
