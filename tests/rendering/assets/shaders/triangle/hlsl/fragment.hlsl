struct FragmentInput {
    float4 position : SV_Position;
    float4 color    : COLOR0;
};

struct FragmentOutput {
    float4 color : SV_Target; 
};

FragmentOutput main(FragmentInput input) {
    FragmentOutput output;
    output.color = input.color;
    return output;
}