// CapsuleAssetProtector動作確認用のピクセルシェーダー
// DiffuseTextureという名前なので、DrawMesh時にモデルのベースカラーが自動で割り当てられる
Texture2D DiffuseTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 lightDirection = normalize(float3(0.4f, -0.8f, 0.5f));
    float lighting = saturate(dot(normalize(input.normal), -lightDirection)) * 0.8f + 0.2f;
    float4 color = DiffuseTexture.Sample(LinearSampler, input.uv);
    return float4(color.rgb * lighting, color.a);
}
