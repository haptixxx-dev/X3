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
    float4 LightPosition; // .xyz used, .w padding
    float4 ViewPosition;  // .xyz used, .w padding
    float4 AmbientColor;  // .xyz used, .w padding
    float4 LightColor;    // .xyz used, .w padding
    float SpecularPower;
    float SpecularStrength;
    float2 _pad;
};

float4 main(PSInput input) : SV_Target0 {
    float3 N = normalize(input.Normal);
    float3 L = normalize(LightPosition.xyz - input.WorldPos);
    float3 V = normalize(ViewPosition.xyz - input.WorldPos);

    // Ambient
    float3 ambient = AmbientColor.xyz;

    // Diffuse
    float diff = max(dot(N, L), 0.0);
    float3 diffuse = LightColor.xyz * diff;

    // Specular (for Phong)
    float3 R = reflect(-L, N);
    float spec = pow(max(dot(V, R), 0.0), SpecularPower);
    float3 specular = LightColor.xyz * spec * SpecularStrength;

    // Combine
    float3 lighting = ambient + diffuse + specular;
    float3 finalColor = input.Color.rgb * lighting;
    
    return float4(finalColor, input.Color.a);
}
