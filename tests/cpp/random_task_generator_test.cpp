// Copyright © 2023 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.
#include "vpu/sample_generator/random_task_generator.h"
#include "vpu/sample_generator/sample_generator.h"
#include "vpu/validation/dpu_operations_sanitizer.h"
#include "vpu/validation/layer_sanitizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

#include "common_helpers.h"

namespace VPUNN_unit_tests {

class SamplerTest : public ::testing::Test {
public:
protected:
    void SetUp() override {
    }

private:
};

/// Test basic case for uniform distribution
TEST_F(SamplerTest, UniformTest) {
    VPUNN::Sampler sampler;

    std::vector<int> src{1, 2, 3, 4, 5};
    const int samples{1000};

    std::vector<int> genout;
    for (int n = 0; n < samples; ++n) {
        auto g = sampler.sample_list(src);
        genout.push_back(g);
    }

    // lets count them

    std::vector<int> histo;
    for (auto item : src) {
        const int num_items = static_cast<int>(std::count(genout.cbegin(), genout.cend(), item));
        histo.push_back(num_items);
    }

    const int average_cnt = samples / static_cast<int>(src.size());
    const int max_dev = static_cast<int>(average_cnt * 0.3F);
    for (const auto& elem : histo) {
        const auto delta = std::abs(elem - average_cnt);
        EXPECT_LE(delta, max_dev) << "generated: " << elem << " times , expected average :" << average_cnt
                                  << " Seed: " << sampler.get_seed();
    }
}

/// Test basic case for decreasing distribution
TEST_F(SamplerTest, DecreasingDistributionTest) {
    VPUNN::Sampler sampler;

    const int bins = 20;
    std::vector<int> src(bins);
    std::iota(src.begin(), src.end(), 1);  // 1..bins

    const int samples{1000};

    std::vector<int> genout;
    for (int n = 0; n < samples; ++n) {
        auto g = sampler.sample_list_decrease_prob(src);
        genout.push_back(g);
    }

    // lets count them

    std::vector<int> histo;
    for (auto item : src) {
        const int num_items = static_cast<int>(std::count(genout.cbegin(), genout.cend(), item));
        histo.push_back(num_items);
    }

    // std::cout << "\n Seed: " << sampler.get_seed();
    //  for (const auto& elem : histo) {
    //      std::cout << "\n generated " << std::setw(4) << elem << " times";
    //  }
    //  std::cout << "\n";

    // expect decreasing probability
    EXPECT_GT(histo[0], histo[1]) << " Seed: " << sampler.get_seed();
    EXPECT_GT(histo[1], histo[2]) << " Seed: " << sampler.get_seed();
    EXPECT_GT(histo[2], *(histo.cend() - 1)) << " Seed: " << sampler.get_seed();
}

class DPU_OperationCreatorTest : public ::testing::Test {
public:
protected:
    void SetUp() override {
    }

private:
};

TEST_F(DPU_OperationCreatorTest, createSanitaryTest) {
    // VPUNN::VPU2_0_DeviceValidValues config_VPU_2_0;
    // VPUNN::VPU2_7_DeviceValidValues config_VPU_2_7;

    {
        VPUNN::DPU_OperationCreator dut;

        VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_0};
        VPUNN::DPUWorkload res{dut.create(device_req)};

        EXPECT_EQ(res.device, device_req);
    }
    {
        VPUNN::DPU_OperationCreator dut;
        VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_7};
        VPUNN::DPUWorkload res{dut.create(device_req)};

        EXPECT_EQ(res.device, device_req);
    }
}
TEST_F(DPU_OperationCreatorTest, createIndirectSanitaryTest) {
    {
        VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_0};
        VPUNN::randDPUWorkload dut(device_req);
        VPUNN::DPUWorkload res{dut()};

        EXPECT_EQ(res.device, device_req);
    }
    {
        VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_7};
        VPUNN::randDPUWorkload dut(device_req);
        VPUNN::DPUWorkload res{dut()};

        EXPECT_EQ(res.device, device_req);
    }
}

/// tests that generated workload do fit in cmx memory
TEST_F(DPU_OperationCreatorTest, checkOcupiedMemoryTest_stochastic) {
    unsigned int n_workloads = 1000;
    VPUNN::DPU_OperationValidator validator;
    VPUNN::DPU_OperationSanitizer sanitizer;
    {
        VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_0};
        VPUNN::randDPUWorkload dut(device_req);

        ASSERT_TRUE(validator.is_supported(device_req));

        const auto& config{validator.get_config(device_req)};

        // Generate N workloads
        auto workloads = std::vector<VPUNN::DPUWorkload>(n_workloads);
        EXPECT_NO_THROW(std::generate_n(workloads.begin(), n_workloads, dut));

        int i{0};  // increments at every error
        int index{0};
        for (auto& wl : workloads) {
            const auto cmx_memory = validator.compute_wl_memory(wl);
            const int avaialable_cmx_memo{config.get_cmx_size(wl.device)};
            const auto necesarry_cmx_memo = cmx_memory.cmx;

            EXPECT_LE(necesarry_cmx_memo, avaialable_cmx_memo) << "WL out of bounds "
                                                               << ". i:" << ++i << std::endl
                                                               << ". idx:" << index << std::endl
                                                               << wl << std::endl
                                                               << " Memory Size : " << cmx_memory << std::endl;
            index++;
        }

        EXPECT_EQ(i, 0) << " Expected all in memory!. Deviations: " << i << " from " << n_workloads << " workloads!"
                        << std::endl;
    }

    {
        VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_7};
        VPUNN::randDPUWorkload dut(device_req);
        ASSERT_TRUE(validator.is_supported(device_req));

        const auto& config{validator.get_config(device_req)};

        // Generate N workloads
        auto workloads = std::vector<VPUNN::DPUWorkload>(n_workloads);
        EXPECT_NO_THROW(std::generate_n(workloads.begin(), n_workloads, dut));

        int i{0};  // increments at every error
        int index{0};
        for (auto& wl : workloads) {
            const auto cmx_memory = validator.compute_wl_memory(wl);
            const int avaialable_cmx_memo{config.get_cmx_size(wl.device)};
            const auto necesarry_cmx_memo = cmx_memory.cmx;

            EXPECT_LE(necesarry_cmx_memo, avaialable_cmx_memo) << "WL out of bounds "
                                                               << ". i:" << ++i << std::endl
                                                               << ". idx:" << index << std::endl
                                                               << wl << std::endl
                                                               << " Memory Size : " << cmx_memory << std::endl;

            VPUNN::SanityReport sane;
            sanitizer.check_data_consistency(wl, sane);

            EXPECT_TRUE(sane.is_usable()) << "WL NOT VALID"
                                          << ". i:" << ++i << std::endl
                                          << ". idx:" << index << std::endl
                                          << wl << std::endl
                                          << " FINDINGS: ----------------------\n"
                                          << sane.info << std::endl
                                          << " END FINDINGS: ---------------------\n";

            index++;
        }

        EXPECT_EQ(i, 0) << " Expected all in memory!. Deviations: " << i << " from " << n_workloads << " workloads!"
                        << std::endl;
    }
}

class DPU_OperationSanitizerTest : public ::testing::Test {
public:
protected:
    void SetUp() override {
    }

private:
};

TEST_F(DPU_OperationSanitizerTest, basicSanitizeTest) {
    VPUNN::DPU_OperationSanitizer dut;
    VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_7};
    VPUNN::SanityReport sane;
    {
        VPUNN::DPUWorkload wl = {
                device_req,
                VPUNN::Operation::CONVOLUTION,
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::UINT8)},  // input dimensions
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::UINT8)},  // output dimensions
                {1, 1},                                                     // kernels
                {1, 1},                                                     // strides
                {0, 0, 0, 0},                                               // padding
                VPUNN::ExecutionMode::CUBOID_16x16                          // execution mode
        };

        dut.check_and_sanitize(wl, sane);

        EXPECT_EQ(sane.value(), V(VPUNN::Cycles::NO_ERROR))
                << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                << wl;
        EXPECT_EQ(wl.inputs[0].get_dtype(), VPUNN::DataType::UINT8);
    }
    {
        VPUNN::DPUWorkload wl = {
                device_req,
                VPUNN::Operation::CONVOLUTION,
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::INT8)},  // input dimensions
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::INT8)},  // output dimensions
                {1, 1},                                                    // kernels
                {1, 1},                                                    // strides
                {0, 0, 0, 0},                                              // padding
                VPUNN::ExecutionMode::CUBOID_16x16                         // execution mode
        };

        dut.check_and_sanitize(wl, sane);

        EXPECT_EQ(sane.value(), V(VPUNN::Cycles::NO_ERROR))
                << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                << wl;
        EXPECT_EQ(wl.inputs[0].get_dtype(), VPUNN::DataType::UINT8);
        EXPECT_EQ(wl.outputs[0].get_dtype(), VPUNN::DataType::UINT8);
    }

    {
        VPUNN::DPUWorkload wl = {
                device_req,
                VPUNN::Operation::CONVOLUTION,
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::BFLOAT16)},  // input dimensions
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::FLOAT16)},   // output dimensions
                {1, 1},                                                        // kernels
                {1, 1},                                                        // strides
                {0, 0, 0, 0},                                                  // padding
                VPUNN::ExecutionMode::CUBOID_16x16                             // execution mode
        };

        dut.check_and_sanitize(wl, sane);

        EXPECT_EQ(sane.value(), V(VPUNN::Cycles::NO_ERROR))
                << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                << wl;
        EXPECT_EQ(wl.inputs[0].get_dtype(), VPUNN::DataType::FLOAT16);
        EXPECT_EQ(wl.outputs[0].get_dtype(), VPUNN::DataType::FLOAT16);
    }

    {
        VPUNN::DPUWorkload wl = {
                device_req,
                VPUNN::Operation::__size,
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::INT8)},  // input dimensions
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::INT8)},  // output dimensions
                {1, 1},                                                    // kernels
                {1, 1},                                                    // strides
                {0, 0, 0, 0},                                              // padding
                VPUNN::ExecutionMode::CUBOID_16x16                         // execution mode
        };

        dut.check_and_sanitize(wl, sane);

        EXPECT_EQ(sane.value(), V(VPUNN::Cycles::ERROR_INVALID_INPUT_OPERATION))
                << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                << wl;
    }
    {
        VPUNN::DPUWorkload wl = {
                device_req,
                VPUNN::Operation::ELTWISE,
                {VPUNN::VPUTensor(1600, 1600, 64, 1, VPUNN::DataType::INT8)},  // input dimensions
                {VPUNN::VPUTensor(1600, 1600, 64, 1, VPUNN::DataType::INT8)},  // output dimensions
                {1, 1},                                                        // kernels
                {1, 1},                                                        // strides
                {0, 0, 0, 0},                                                  // padding
                VPUNN::ExecutionMode::CUBOID_16x16                             // execution mode
        };

        dut.check_and_sanitize(wl, sane);

        EXPECT_EQ(sane.value(), V(VPUNN::Cycles::ERROR_INPUT_TOO_BIG))
                << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                << wl;
    }

    device_req = VPUNN::VPUDevice::VPU_2_0;
    {
        VPUNN::DPUWorkload wl = {
                device_req,
                VPUNN::Operation::CONVOLUTION,
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::INT8, VPUNN::Layout::ZMAJOR)},  // input dimensions
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::INT8, VPUNN::Layout::ZMAJOR)},  // output dimensions
                {1, 1},                                                                           // kernels
                {1, 1},                                                                           // strides
                {0, 0, 0, 0},                                                                     // padding
                VPUNN::ExecutionMode::VECTOR                                                      // execution mode
        };

        dut.check_and_sanitize(wl, sane);

        EXPECT_EQ(sane.value(), V(VPUNN::Cycles::NO_ERROR))
                << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                << wl;
        EXPECT_EQ(wl.inputs[0].get_dtype(), VPUNN::DataType::UINT8);
        EXPECT_EQ(wl.outputs[0].get_dtype(), VPUNN::DataType::UINT8);
    }

    {
        VPUNN::DPUWorkload wl = {
                device_req,
                VPUNN::Operation::CONVOLUTION,
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::BFLOAT16,
                                  VPUNN::Layout::ZMAJOR)},  // input dimensions
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::FLOAT16,
                                  VPUNN::Layout::ZMAJOR)},  // output dimensions
                {1, 1},                                     // kernels
                {1, 1},                                     // strides
                {0, 0, 0, 0},                               // padding
                VPUNN::ExecutionMode::VECTOR                // execution mode
        };

        dut.check_and_sanitize(wl, sane);

        EXPECT_EQ(sane.value(), V(VPUNN::Cycles::NO_ERROR))
                << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                << wl;
        EXPECT_EQ(wl.inputs[0].get_dtype(), VPUNN::DataType::FLOAT16);
        EXPECT_EQ(wl.outputs[0].get_dtype(), VPUNN::DataType::FLOAT16);
    }
}

class LayersValidationTest : public ::testing::Test {
public:
protected:
    void SetUp() override {
    }

private:
};

TEST_F(LayersValidationTest, basicLayerValidatorTest) {
    VPUNN::LayersValidation dut;
    VPUNN::VPUDevice device_req{VPUNN::VPUDevice::VPU_2_7};

    {
        VPUNN::DPULayer wl(VPUNN::DPUWorkload{
                device_req,
                VPUNN::Operation::CONVOLUTION,
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::UINT8)},  // input dimensions
                {VPUNN::VPUTensor(16, 16, 64, 1, VPUNN::DataType::UINT8)},  // output dimensions
                {1, 1},                                                     // kernels
                {1, 1},                                                     // strides
                {0, 0, 0, 0},                                               // padding
                VPUNN::ExecutionMode::CUBOID_16x16                          // execution mode
        });

        {
            VPUNN::SanityReport sane;
            dut.check_completeLayer_consistency(wl, sane, VPUNN::ISIStrategy::CLUSTERING, 1);

            EXPECT_EQ(sane.value(), V(VPUNN::Cycles::NO_ERROR))
                    << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                    << wl;
        }

        {
            VPUNN::SanityReport sane;
            dut.check_splitLayer_consistency(wl, sane);

            EXPECT_EQ(sane.value(), V(VPUNN::Cycles::NO_ERROR))
                    << sane.info << "\n error is : " << VPUNN::Cycles::toErrorText(sane.value()) << "\n"
                    << wl;
        }
    }
}

}  // namespace VPUNN_unit_tests
