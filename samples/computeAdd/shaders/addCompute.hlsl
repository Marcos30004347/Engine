[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> storageBuffer :  register(u0, space0);

[[vk::binding(0, 1)]]
cbuffer AddValueCB : register(b1, space0)
{
    uint addValue;
}

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint idx = DTid.x;
    storageBuffer[idx] += addValue; 
}