cbuffer Frame : register(b0)
{
    float4x4 viewProj;
    float3   lightDir;   // world space, points from surface towards light
    float    _pad;
};

cbuffer Material : register(b1)
{
    float3 matColor;
    float  hasTexture;
    float  alphaTest;
    float3 _pad2;
};

Texture2D    albedoTex : register(t0);
SamplerState albedoSmp : register(s0);

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;     // UNORM-packed
    float2 uv     : TEXCOORD;
};

struct VSOut
{
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

VSOut vs_main(VSIn i)
{
    VSOut o;
    o.pos    = mul(viewProj, float4(i.pos, 1.0));
    o.normal = i.normal * 2.0 - 1.0;
    o.uv     = i.uv;
    return o;
}

float4 ps_main(VSOut i, bool front : SV_IsFrontFace) : SV_Target
{
    float4 albedo = hasTexture > 0 ? albedoTex.Sample(albedoSmp, i.uv) : float4(matColor, 1.0);
    if (alphaTest > 0) clip(albedo.a - 0.5);

    float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    float  d = saturate(dot(n, lightDir));
    return float4(albedo.rgb * (0.3 + 0.7 * d), 1.0);
}
