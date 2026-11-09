// Resolve exponent bias (RB_COPY_DEST_INFO copy_dest_exp_bias): Xenos resolves
// scale the copied colour by 2^bias (the sun's shadow map: -5, the k_16_16
// target's -32..32 becomes -1..1 in the texture). The host scales the source
// target in place around the copy with blending (destination x blend factor),
// so this shader's output is multiplied by zero and only has to exist.
float4 PSMain() : SV_Target {
  return 0.0;
}
