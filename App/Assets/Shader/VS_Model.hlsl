// CapsuleAssetProtector動作確認用の頂点シェーダー
// 入力はPOSITION/NORMAL/TEXCOORDだけを宣言している(CAPが頂点レイアウトを自動で合わせる)
cbuffer Transform : register(b0)
{
    float4x4 world;
    float4x4 viewProjection;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPosition, viewProjection);
    output.normal = normalize(mul(input.normal, (float3x3)world));
    output.uv = input.uv;
    return output;
}
