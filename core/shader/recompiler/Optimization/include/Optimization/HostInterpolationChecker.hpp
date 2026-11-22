#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HOSTINTERPOLATIONCHECKER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_HOSTINTERPOLATIONCHECKER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <cstdint>

namespace ShaderRecompiler {

class HostInterpolationChecker {
public:
    [[nodiscard]] bool Lower(IrProgram& program, const ShaderPixelInputInfo& pixel, bool barycentricBuiltins, bool firstVertexFlat) const;
    [[nodiscard]] std::uint32_t HostInterpolatedInputs(const IrProgram& program, const ShaderPixelInputInfo& pixel) const;
    [[nodiscard]] bool LowerHostInputs(IrProgram& program, const ShaderPixelInputInfo& pixel, std::uint32_t& failedInputs) const;
};

}

#endif
