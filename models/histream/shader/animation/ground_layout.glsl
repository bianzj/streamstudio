// Shared std430 layout for the optional local ground EB correction.
struct GroundMaterial{vec4 energy;vec4 flags;}; // alpha,eps,C,k; canopy,receiver,0,0
struct GroundCell{vec4 point,sky,emission,solar,state;};
layout(std430,binding=GROUND_PARAMS_BINDING)readonly buffer GroundParameters{
    ivec4 groundSize;vec4 groundDomain,groundSettings,groundControl;GroundMaterial groundMaterials[];
};
#ifndef GROUND_FIELD_ACCESS
#define GROUND_FIELD_ACCESS
#endif
layout(std430,binding=GROUND_FIELD_BINDING)GROUND_FIELD_ACCESS buffer GroundCells{GroundCell groundCells[];};
