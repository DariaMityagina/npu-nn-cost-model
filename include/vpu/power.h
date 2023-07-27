// Copyright © 2023 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.

#ifndef VPUNN_POWER_H
#define VPUNN_POWER_H

#include <cmath>
#include <list>
#include <map>
#include <tuple>
#include "vpu/performance.h"
#include "vpu/types.h"
#include "vpu/utils.h"
#include "vpunn.h"

namespace VPUNN {

/**
 * @brief A structure that contains Dynamic Frequency and Voltage Scaling (DVFS) points for the VPU
 *
 */
struct DVFS {
    /**
     * @brief DVFS Voltage in Volt
     *
     */
    float voltage;
    /**
     * @brief DVFS Frequency in MHz
     *
     */
    float frequency;  // MHz
};

/**
 * @brief VPU Power factor LUTs
 * @details The power factor LUT is lookup table that will be indexed by operation
 * and will return another LUT that will be indexed by the number of input channels
 * When there is no entry in the second LUT, the value returned will be the interpolation between its smaller and
 * greater match in table
 */
class VPUPowerFactorLUT {
private:
    unsigned int input_ch;
    Operation op_type;
    VPUDevice vpu_device;
    typedef std::vector<std::tuple<Operation, std::map<unsigned int, float>>> lut_t;
    // unsigned int key represents the number of input channels
    // and the float value represents the power factor calculated based on simulation measurements
    std::vector<std::tuple<VPUDevice, lut_t>> pf_lut;

    void initialize_pf_lut() {
        lut_t vpu_2_7_values;
        lut_t vpu_2_0_values;
        lut_t vpu_4_0_values;

        // VPU2.0 values (Op type: {log2(input_channels): power_factor}))
        vpu_2_0_values.push_back({Operation::CONVOLUTION,
                                  {
                                          {4, 0.87f},
                                          {5, 0.92f},
                                          {6, 1.0f},
                                          {7, 0.95f},
                                          {8, 0.86f},
                                          {9, 0.87f},
                                  }});
        vpu_2_0_values.push_back({Operation::DW_CONVOLUTION,
                                  {
                                          {6, 5.84f},
                                  }});
        vpu_2_0_values.push_back({Operation::AVEPOOL,
                                  {
                                          {6, 32.60f},
                                  }});
        vpu_2_0_values.push_back({Operation::MAXPOOL,
                                  {
                                          {6, 5.29f},
                                  }});
        vpu_2_0_values.push_back({Operation::ELTWISE,
                                  {
                                          {7, 232.71f},
                                  }});

        // VPU2.7 values (Op type: {log2(input_channels): power_factor}))
        vpu_2_7_values.push_back({Operation::CONVOLUTION,
                                  {
                                          {4, 1.97f},
                                          //{5, 1.11f},
                                          //{6, 1.16f},
                                          {7, 1.20f},
                                          {8, 1.08f},
                                          {9, 1.07f},
                                          {10, 1.01f},
                                          {11, 0.97f},
                                  }});
        vpu_2_7_values.push_back({Operation::DW_CONVOLUTION,
                                  {
                                          {6, 1.43f},
                                  }});
        vpu_2_7_values.push_back({Operation::AVEPOOL,
                                  {
                                          {6, 0.29f},
                                  }});
        vpu_2_7_values.push_back({Operation::MAXPOOL,
                                  {
                                          {6, 1.15f},
                                  }});
        vpu_2_7_values.push_back({Operation::ELTWISE,
                                  {
                                          {8, 0.11f},
                                  }});

        // TODO: add VPU 4.0 values
        pf_lut.push_back({VPUDevice::VPU_2_0, vpu_2_0_values});
        pf_lut.push_back({VPUDevice::VPU_2_7, vpu_2_7_values});
    }

    float getScaledValue(float value, bool fp_comp, VPUDevice device) const {
        float scaled_value = value;
        if ((device == VPUDevice::VPU_2_0) && fp_comp)
            scaled_value = value * 0.87f;
        else if ((device == VPUDevice::VPU_2_7) && !fp_comp)
            scaled_value = value * 0.79f;
        return scaled_value;
    }

    float getValueInterpolation(const std::map<unsigned int, float>& table) const {
        float interp_value = 0;

        // Get the smaller and greater neighbour
        const unsigned int max_ch_log2 = (unsigned int)ceil(log2(8192));  // Max input channels
        unsigned int smaller = 0;
        unsigned int greater = max_ch_log2;

        float input_ch_log2 = log2((float)input_ch);

        for (auto it = table.begin(); it != table.end(); ++it) {
            // Find the index below or at input_ch
            if (((float)it->first <= input_ch_log2) && (it->first > smaller))
                smaller = it->first;

            // Find the index above or at input_ch
            if (((float)it->first >= input_ch_log2) && (it->first < greater))
                greater = it->first;

            if (smaller == greater)
                break;
        }

        const float interval = (float)(greater - smaller);
        if (interval > 0) {
            // Logarithmic interpolation between entries
            interp_value = ((float)greater - input_ch_log2) / interval * table.at(smaller) +
                           (input_ch_log2 - (float)smaller) / interval * table.at(greater);
        } else {
            // Direct hit
            interp_value = table.at(smaller);
        }
        return interp_value;
    }

public:
    /**
     * @brief Construct a new VPUPowerFactorLUT object
     *
     * @param input_ch the LUT reference input channel size
     * @param op_type the LUT reference Operation type
     * @param vpu_device the LUT reference VPUDevice
     */
    VPUPowerFactorLUT(unsigned int input_ch = 16, Operation op_type = Operation::CONVOLUTION,
                      VPUDevice vpu_device = VPUDevice::VPU_2_7)
            : input_ch(input_ch), op_type(op_type), vpu_device(vpu_device) {
        initialize_pf_lut();
    };

    /**
     * @brief Get the the value from the LUT for a specific datatype
     *
     * @param fp_comp true if floating point native compute required for workload
     * @return float the LUT value for that datatype
     */
    float getValue(bool fp_comp) const {
        float pf_value = 0;
        lut_t device_table{};  // empty vector
        // Get values table for the device
        for (auto i = pf_lut.begin(); i != pf_lut.end(); ++i)
            if (std::get<0>(*i) == vpu_device)
                device_table = std::get<1>(*i);  // makes a copy(optimization possible!)

        // Get the power factor value
        for (auto i = device_table.begin(); i != device_table.end(); ++i) {
            const Operation operation = std::get<0>(*i);
            const std::map<unsigned int, float>& op_values_map = std::get<1>(*i);

            if (operation == op_type) {
                pf_value = getScaledValue(getValueInterpolation(op_values_map), fp_comp, vpu_device);
            }
        }
        return pf_value;
    }
};

/**
 * @brief VPUNN power model
 *
 */
class VPUNN_API(VPUNNPowerModel) {
public:
    /**
     * @brief Get the valid DVFS points for a specific VPUDevice
     *
     * @param device a VPUDevice
     * @return std::list<DVFS> valid DVFS points
     */
    std::list<DVFS> getValidDVFS(VPUDevice& device) {
        switch (device) {
        case VPUDevice::VPU_2_0:
            return {{0.8f, 700}};
        case VPUDevice::VPU_2_1:
            return {{0.8f, 850}};
        case VPUDevice::VPU_2_7:
            return {{0.6f, 850}, {0.75f, 1100}, {0.9f, 1300}};
        case VPUDevice::VPU_4_0:
            return {{0.55f, 950}, {0.65f, 1550}, {0.75f, 1700}, {0.85f, 1850}};
        default:
            return {{}};
        }
    }

    /**
     * @brief Get the default DVFS point for a specific VPUDevice
     *
     * @param device a VPUDevice
     * @return DVFS the default DVFS point for a specific VPUDevice
     */
    DVFS getDefaultDVFS(VPUDevice& device) {
        auto validDVFS = getValidDVFS(device);
        auto maxElem = *std::max_element(validDVFS.begin(), validDVFS.end(), [](const DVFS& a, const DVFS& b) {
            return a.frequency < b.frequency;
        });

        return maxElem;
    }

    /**
     * @brief Get the DPU default frequency in MHz
     *
     * @param device a VPUDevice
     * @return float
     */
    inline float getDefaultVoltage(VPUDevice& device) {
        auto dvfs = getDefaultDVFS(device);
        return dvfs.voltage;
    }

    /**
     * @brief Get the dynamic power from C_dyn, activity factor and DVFS
     *
     * @param c_dyn power parameter
     * @param activity_factor workload activity factor
     * @param dvfs voltage and frequency
     * @return float dynamic power
     */
    float DynamicPower(float c_dyn, float activity_factor, DVFS dvfs) {
        return DynamicPower(c_dyn, activity_factor, dvfs.voltage, dvfs.frequency);
    }

    /**
     * @brief Get the dynamic power from C_dyn, activity factor and DVFS
     *
     * @param c_dyn power parameter
     * @param activity_factor workload activity factor
     * @param voltage in V
     * @param frequency in MHz
     * @return float dynamic power
     */
    float DynamicPower(float c_dyn, float activity_factor, float voltage, float frequency) {
        return c_dyn * frequency * voltage * voltage * activity_factor;
    }

    /**
     * @brief Compute the power (in mW) of a DMAWorkload
     *
     * @param wl a DMAWorkload
     * @return float DMAWorkload consumed power in mW
     */
    float DMAPower(DMAWorkload wl) {
        return DMAPower(wl, getDefaultDVFS(wl.device));
    }

    /**
     * @brief Compute the power (in mW) of a DMAWorkload
     *
     * @param wl a DMAWorkload
     * @param dvfs voltage and frequency
     * @return float DMAWorkload consumed power in mW
     */
    float DMAPower(DMAWorkload& wl, DVFS dvfs) {
        const float activity_factor = 1.0f;
        float c_dyn = getCDyn(wl.device, VPUSubsystem::VPU_DMA);
        return DynamicPower(c_dyn, activity_factor, dvfs);
    }

    /**
     * @brief Compute the power (in mW) of a DMAWorkload
     *
     * @param device DMA VPUDevice
     * @param input DMA input Tensor
     * @param output DMA output Tensor
     * @param input_location where is the source memory
     * @param output_location where is the destination memory
     * @param output_write_tiles  how many CMX tiles the DMA broadcast
     * @return float DMAWorkload consumed power in mW
     */
    float DMAPower(VPUDevice device, const VPUTensor& input, const VPUTensor& output,
                   MemoryLocation input_location = MemoryLocation::DRAM,
                   MemoryLocation output_location = MemoryLocation::CMX, unsigned int output_write_tiles = 1) {
        // Call the helper function
        return DMAPower({device, input, output, input_location, output_location, output_write_tiles});
    }

    /**
     * @brief return the CDyn in nF for a specific VPUSubsystem
     *
     * @param device VPUDevice
     * @param hw the CDyn
     * @return float
     */
    float getCDyn(VPUDevice& device, VPUSubsystem hw) {
        switch (device) {
        case VPUDevice::VPU_2_0:
            return getCDyn_2_0(hw);
        case VPUDevice::VPU_2_1:
            return getCDyn_2_1(hw);
        case VPUDevice::VPU_2_7:
            return getCDyn_2_7(hw);
        case VPUDevice::VPU_4_0:
            return getCDyn_4_0(hw);
        default:
            return 0;
        }
    }

    /**
     * @brief Return the static power for any VPUSubsystem of any VPUDevice
     *
     * @param device VPUDevice
     * @param hw VPUSubsystem
     * @return float static power
     */
    float StaticPower(VPUDevice& device, VPUSubsystem hw) {
        auto dvfs = getDefaultDVFS(device);
        return StaticPower(device, hw, dvfs);
    }

    /**
     * @brief Return the static power for any VPUSubsystem of any VPUDevice
     *
     * @param device VPUDevice
     * @param hw VPUSubsystem
     * @param dvfs voltage and frequency
     * @return float static power
     */
    float StaticPower(VPUDevice& device, VPUSubsystem hw, DVFS dvfs) {
        float nominal_leakage = getNominalLeakage(device, hw);
        float nominal_voltage = getDefaultVoltage(device);
        return nominal_leakage * dvfs.voltage / nominal_voltage;
    }

    /**
     * @brief Get the nominal leakage object any VPUSubsystem of any VPUDevice
     *
     * @param device VPUDevice
     * @param hw VPUSubsystem
     * @return float nominal leakage
     */
    float getNominalLeakage(VPUDevice& device, VPUSubsystem hw) {
        switch (device) {
        case VPUDevice::VPU_2_0:
            return getLeakage_2_0(hw);
        case VPUDevice::VPU_2_1:
            return getLeakage_2_1(hw);
        case VPUDevice::VPU_2_7:
            return getLeakage_2_7(hw);
        case VPUDevice::VPU_4_0:
            return getLeakage_4_0(hw);
        default:
            return 0;
        }
    }

    // ##########################################################
    // ################## HW SPECIFIC CONSTANTS #################
    // ##########################################################
    // TODO: fill hw specific constants

    /**
     * @brief Get the CDyn for VPU_2_0
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getCDyn_2_0(VPUSubsystem hw) {
        // TODO: fill values for 2_0
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }

    /**
     * @brief Get the CDyn for VPU_2_1
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getCDyn_2_1(VPUSubsystem hw) {
        // TODO: fill values for 2_1
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }

    /**
     * @brief Get the CDyn for VPU_2_7
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getCDyn_2_7(VPUSubsystem hw) {
        // TODO: fill values for 2_7
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }

    /**
     * @brief Get the CDyn for VPU_2_1
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getCDyn_4_0(VPUSubsystem hw) {
        // TODO: fill values for 4_0
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }

    /**
     * @brief Get the leakage for VPU_2_0
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getLeakage_2_0(VPUSubsystem hw) {
        // TODO: fill values for 2_0
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }

    /**
     * @brief Get the leakage for VPU_2_1
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getLeakage_2_1(VPUSubsystem hw) {
        // TODO: fill values for 2_1
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }

    /**
     * @brief Get the leakage for VPU_2_7
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getLeakage_2_7(VPUSubsystem hw) {
        // TODO: fill values for 2_7
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }

    /**
     * @brief Get the leakage for VPU_2_0
     *
     * @param hw VPUSubsystem
     * @return float
     */
    inline float getLeakage_4_0(VPUSubsystem hw) {
        // TODO: fill values for 4_0
        switch (hw) {
        case VPUSubsystem::VPU_DPU:
            return 0;
        case VPUSubsystem::VPU_DMA:
            return 0;
        case VPUSubsystem::VPU_SHV:
            return 0;
        default:
            return 0;
        }
    }
};

}  // namespace VPUNN

#endif  // VPUNN_POWER_H
