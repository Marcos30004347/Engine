RWStructuredBuffer<uint> storageBuffer : register(u0);

cbuffer AddValueCB : register(b1)
{
    uint addValue;
}

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint idx = DTid.x;
    
    // The shader now reads the value from the uniform buffer
    storageBuffer[idx] += addValue; 
}