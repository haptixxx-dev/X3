struct PSInput {
    float4 Position : SV_Position;
    float3 WorldPos : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Color : COLOR0;
};

cbuffer UniformBlock : register(b0) {
    float4x4 ViewProjection; // unused in pixel shader but kept for layout
    float4x4 Model;
    float4 Color;
    float3 LightPosition;
    float3 ViewPosition;
    float3 AmbientColor;
    float3 LightColor;
    float SpecularPower;
    float SpecularStrength;
};

float4 main(PSInput input) : SV_Target0 {
    float3 N = normalize(input.Normal);
    float3 L = normalize(LightPosition - input.WorldPos);
    float3 V = normalize(ViewPosition - input.WorldPos);

    // diffuse term
    float diff = max(dot(N, L), 0.0);

    // specular term (Phong)
    float3 R = reflect(-L, N);
    float spec = 0.0;
    if (diff > 0.0) {
        spec = pow(max(dot(V, R), 0.0), max(1.0, SpecularPower)) * SpecularStrength;
    }

    float3 baseColor = input.Color.rgb;
    float3 ambient = AmbientColor * baseColor;
    float3 diffuse = LightColor * diff * baseColor;
    float3 specular = LightColor * spec;

    float3 finalColor = ambient + diffuse + specular;
    return float4(finalColor, input.Color.a);
}
