// Copyright © 2023 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.

#ifndef VPUNN_VPU_INTERFACE_OPERATIONS_BEHAVIOR_H
#define VPUNN_VPU_INTERFACE_OPERATIONS_BEHAVIOR_H

#include "data_dpu_operation.h"
#include "vpu/types.h"

namespace VPUNN {

// fw declaration of valid values interface
class IDeviceValidValues;

/// @brief Interface class for constraints/behaviors that are specific to operations
/// It enforces dynamically the workload setup. Derived classes will implement specific rules based on the operation
class IOperationDynamicConstraints {
public:
    /// @brief computes size of weights (input_1) in elements not bytes
    virtual long long input_1_volume(const TensorInfo& w) const noexcept = 0;

    /// @brief computes the aligned size in bytes
    virtual long long input_1_aligned_size_bytes(const long long elem_size, const IDeviceValidValues& config,
                                                 const DPUOperation& dpu) const noexcept = 0;

    /// @brief computes size of activators (input_0)
    virtual long long input_0_volume(const TensorInfo& w) const noexcept {
        return w.height * w.width * w.channels;
    };

    /// @brief computes size of activators (input_0)
    long long output_0_volume(const TensorInfo& w) const noexcept {
        return w.height * w.width * w.channels;
    };

    /// @brief deduce input_1 based on input_0 and output_0,
    /// deduce the weights
    virtual void deduce_input_1(const TensorInfo& in_0, const TensorInfo& out_0, const IDeviceValidValues& config,
                                const KernelInfo& kernel, TensorInfo& w) const noexcept = 0;

    /// @returns a filtered strategy container that has the invalid ones eliminated. Operation dependent.
    virtual Values<ISIStrategy> filter_ISI_Strategy_Options(const Values<ISIStrategy>& strategies) const {
        return strategies;
    }

    /// @returns a output_write_tile container that has the invalid ones eliminated. Operation dependent.
    virtual Values<int> filter_output_write_tile_Options(const Values<int>& output_write_tile_variants) const {
        return output_write_tile_variants;  // nothing changed by default
    }

    /// @brief changes kernels in case a stricter constraint must be used
    /// @returns true if normalization was done (kernel changed)
    virtual bool normalize_kernel_dimension(const ISIStrategy&, KernelInfo&) const {
        return false;
    }

    /// @ reduces/adjusts sparsity  according to context
    virtual void limit_sparsity(const IDeviceValidValues& /*config*/, DPUOperation& /*dpu*/) const {};

    /// @brief checks that the sizes of inputs and output tensors are good given the operation.
    virtual bool check_input_output_tensor_corelation(const IDeviceValidValues& config, const DPUOperation& dpu,
                                                      std::string& info) const = 0;

    /// @brief checks that the sparsity respects operation constraints
    virtual bool check_sparsity_rules(const IDeviceValidValues& config, const DPUOperation& dpu,
                                      std::string& info) const = 0;

protected:
    virtual ~IOperationDynamicConstraints() = default;
};

/// @brief interface to a container of IOperationDynamicConstraints associated 1-1 to operations
class IContainer_OperationsDynamicBehavior {
public:
    virtual const IOperationDynamicConstraints& get_operation_specific_behaviour(const Operation op) const = 0;

protected:
    virtual ~IContainer_OperationsDynamicBehavior() = default;
};

}  // namespace VPUNN

#endif  //
