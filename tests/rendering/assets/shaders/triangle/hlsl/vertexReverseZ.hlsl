cbuffer CameraUBO : register(b0)
{
    column_major float4x4 view;
    column_major float4x4 proj;
};

struct VertexInput
{
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;

    float4 world = float4(input.position, 1.0f);

    float4 viewPos = mul(view, world);
    float4 clipPos = mul(proj, viewPos);

    output.position = clipPos;
    output.color = input.color;
    return output;
}