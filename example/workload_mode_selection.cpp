// Copyright © 2022 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.

#include <stdio.h>
#include <vpu/optimization/select_best_split.h>
#include <vpu/optimization/select_optimal_execution_mode.h>
#include <vpu/types.h>
#include <vpu_cost_model.h>

using namespace VPUNN;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage %s <model.vpunn>\n", argv[0]);
        return 0;
    }
    std::string model_path = std::string(argv[1]);
    printf("Loading model from %s\n", model_path.c_str());
    auto model = VPUCostModel(model_path);

    // Example 1: 3x3 convolution, getting the best MPE mode
    printf("=================== Test 1 ===================\n");
    printf("Selecting optimal MPE mode for 3x3s1 CONV\n");
    auto input_tensor = VPUTensor({56, 56, 16, 1}, DataType::UINT8);
    auto output_tensor = VPUTensor({56, 56, 16, 1}, DataType::UINT8);
    auto layer = DPULayer(VPUDevice::VPU_2_0,      //
                          Operation::CONVOLUTION,  //
                          {input_tensor},          // input_0
                          //{input_tensor},          // input_1, @todo: review if it is OK
                          {output_tensor},  // output
                          {3, 3},           // kernels
                          {1, 1},           // strides
                          {1, 1, 1, 1}      // padding
    );
    auto optimal_mode = select_optimal_execution_mode(model, layer);

    switch (optimal_mode) {
    case ExecutionMode::VECTOR:
        printf("Optimal mode is ExecutionMode::VECTOR\n");
        break;
    case ExecutionMode::MATRIX:
        printf("Optimal mode is ExecutionMode::MATRIX\n");
        break;
    case ExecutionMode::VECTOR_FP16:
        printf("Optimal mode is ExecutionMode::VECTOR_FP16\n");
        break;
    default:
        printf("Optimal mode is not a valid VPU_2_0 one\n");
        break;
    }

    printf("=================== Test 2 ===================\n");
    printf("Selecting optimal split strategy for a 3x3s1 CONV\n");
    // Example N2: Giving a 56x56x16 tensor, create multiple splits
    // SPLIT 1: 1 workload covering the entire tensor
    auto wl_full = VPUWorkloadSplit(VPUTensor({56, 56, 16, 1}, DataType::UINT8),
                                    VPUTensor({56, 56, 16, 1}, DataType::UINT8), ExecutionMode::VECTOR);

    // SPLIT 2: 2 workloads
    auto wl_half = VPUWorkloadSplit(VPUTensor({56, 28, 16, 1}, DataType::UINT8),
                                    VPUTensor({56, 28, 16, 1}, DataType::UINT8), ExecutionMode::MATRIX);

    // SPLIT 3: 5 workloads 4x (56x12x16) + (56x8x16)
    auto wl_3_1 = VPUWorkloadSplit(VPUTensor({56, 12, 16, 1}, DataType::UINT8),
                                   VPUTensor({56, 12, 16, 1}, DataType::UINT8), ExecutionMode::MATRIX);
    auto wl_3_2 = VPUWorkloadSplit(VPUTensor({56, 8, 16, 1}, DataType::UINT8),
                                   VPUTensor({56, 8, 16, 1}, DataType::UINT8), ExecutionMode::MATRIX);

    auto optimal_split = select_optimal_split(model, 5, VPUDevice::VPU_2_0, Operation::CONVOLUTION,
                                              {{wl_full}, {wl_half, wl_half}, {wl_3_1, wl_3_1, wl_3_1, wl_3_1, wl_3_2}},
                                              {3, 3}, {1, 1}, {1, 1, 1, 1});

    printf("The optimal split is the n %d (cost: %d cycles)\n", std::get<0>(optimal_split) + 1,
           std::get<1>(optimal_split));

    return 0;
}
