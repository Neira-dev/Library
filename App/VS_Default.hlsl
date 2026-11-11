struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput main(float3 position : POSITION)
{
    VSOutput output;
    output.position = float4(position, 1.0);
    return output;
}