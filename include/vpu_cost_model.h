// Copyright © 2023 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.

#ifndef VPU_COST_MODEL_H
#define VPU_COST_MODEL_H

#include <algorithm>
#include <functional>
#include <numeric>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/cache.h"
#include "core/logger.h"

#include "inference/preprocessing.h"
#include "inference/preprop_factory.h"

#include "vpu/performance.h"
#include "vpu/power.h"
#include "vpu/types.h"
#include "vpu/utils.h"
#include "vpu/validation/checker_utils.h"
#include "vpunn.h"

#include "vpu/cycles_interface_types.h"
#include "vpu/validation/dpu_operations_sanitizer.h"

#include "vpu/shave/shave_collection.h"
#include "vpu/shave/shave_devices.h"

namespace VPUNN {

/// @brief L1API info for a DPUWorkload.
/// intention is to obtain all info at once, in a more efficient way.
/// Zero values means either error or value could not be obtained
/// See the original interface method for each field to understand its meaning
struct DPUInfoPack {
    CyclesInterfaceType DPUCycles{0};  ///< DPU()
    std::string errInfo;               ///< error info when doing DPU()

    float energy{0};  ///< DPUEnergy(), uses power_* information

    // for power usage, considers HW optimization like sparsity
    float power_activity_factor{0};  ///< AF, operation adjusted, INT/FLOAT powerVirus as reference  is important
    float power_mac_utilization{0};  ///< hw_utilization, mac only based, Uses estimated dpu Cycles,
    unsigned long int power_ideal_cycles{0};     ///< pure mac, ops considers sparsity
    unsigned long int sparse_mac_operations{0};  ///< how many macs this operation will have on this hardware

    // efficiency part do not consider HW optimization like sparsity
    float efficiency_activity_factor{0};  ///<  operation adjusted, INT/FLOAT powerVirus as reference  is important
    float efficiency_mac_utilization{0};  ///< no op dependency, mac only based, Uses estimated dpu Cycles
    unsigned long int efficiency_ideal_cycles{0};  ///< pure MAC based
    unsigned long int dense_mac_operations{0};     ///< how many macs this operation will have, mathematical maximum

    unsigned long int hw_theoretical_cycles{0};  ///< DPUTheoreticalCycles
};

inline std::ostream& operator<<(std::ostream& stream, const VPUNN::DPUInfoPack& d) {
    stream << "DPUInfoPack: \n"                                                                       //
           << " DPUCycles: \t" << d.DPUCycles << " : " << Cycles::toErrorText(d.DPUCycles) << " ;\n"  //
           << " errInfo: \t" << d.errInfo << " ;\n"                                                   //
           << " energy: \t" << d.energy << " ;\n"                                                     //

           << " power_activity_factor: \t" << d.power_activity_factor << " ;\n"  //
           << " power_mac_utilization: \t" << d.power_mac_utilization << " ;\n"  //
           << " power_ideal_cycles: \t" << d.power_ideal_cycles << " ;\n"        //
           << " sparse_mac_operations: \t" << d.sparse_mac_operations << " ;\n"  //

           << " efficiency_activity_factor: \t" << d.efficiency_activity_factor << " ;\n"  //
           << " efficiency_mac_utilization: \t" << d.efficiency_mac_utilization << " ;\n"  //
           << " efficiency_ideal_cycles: \t" << d.efficiency_ideal_cycles << " ;\n"        //
           << " dense_mac_operations: \t" << d.dense_mac_operations << " ;\n"              //

           << " hw_theoretical_cycles: \t" << d.hw_theoretical_cycles << " ;\n"  //
            ;
    return stream;
}

/**
 * @brief The VPUCostModel class
 *
 * Has behind a loaded CostModel neural network that infers cycle times for DPUWOrkloads
 *
 */
class VPUNN_API(VPUCostModel): public VPUNNPerformanceModel {
private:
    const RuntimeProcessingFactory preprocessing_factory;  ///< provides Preprocessing objects
    Runtime vpunn_runtime;                ///< the loaded inference model is here, used for FW propagation
    Preprocessing<float>& preprocessing;  ///< prepares the input vector for the runtime, configured at ctor
    LRUCache<float> cache;                ///< cache for inferred values, used only for single workload, not for batches
    const PostProcessSupport results_config;  ///< in case we have a deprecated version with hw_overhead, the ouptut
                                              ///< support should false and the response should be unused

    DPU_OperationSanitizer sanitizer;  ///< sanitizer mechanisms

    const size_t prealloc_results{1000};          ///< how much results buffer to pre-alloc
    std::vector<float> workloads_results_buffer;  ///< buffer dedicated for workloads. avoids reallocation.
    const float default_NN_output{-1.0F};      ///< this is the value used in no NN output is present (like not loaded).
    const VPUPowerFactorLUT power_factor_lut;  /// < this is the lookup table for power factors.

    const ShaveConfiguration shave_gen_2;  ///< second generation of shaves

    /// @brief obtains the actual preprocessing instance from factory. The factory must live longer than the instance
    /// created. warning: Throws if not possible
    static Preprocessing<float>& init_preproc(const RuntimeProcessingFactory& factory,
                                              const ModelVersion& version_service, const std::string& filename) {
        // let's initialize the preproc aspects based on input version
        const int input_version = version_service.get_input_interface_version();
        //  checking if either we have some preprocess or we have a unsupported version
        if (factory.exists_preprocessing(input_version)) {
            auto& preproc = factory.make_preprocessing(input_version);
            return preproc;
        } else {
            std::stringstream buffer;
            buffer << "Cannot create preprocessing stage!.Preprocessing with version (" << input_version
                   << ") is not known/supported. Filename: " << filename
                   << " , Version info (raw): " << version_service.get_raw_name();
            std::string details = buffer.str();
            Logger::error() << details;

            throw std::runtime_error(details);
        }
    }

    /**
     * @brief check post config will check if we either have an empty model, or the output version is supported
     *
     * If we have an empty ideal model or we got the output version supported this function will be exited with
     * no errors. In case that we got a model with an unsupported output version this function will throw a runtime
     * error specifying the output version and the full raw name of it.
     *
     * @throws std::runtime_error In case that we got a model with an unsupported output version you will get a runtime
     * error
     *
     * @param v is the model version we took info about the full_raw_name in case we have an empty model
     */
    void check_post_config(const ModelVersion& v) {
        const auto raw_name_intf = v.get_raw_name();

        // in case we have an empty ideal model the raw_name is defaulted to none and we should contiune the run
        if (raw_name_intf == "none") {
            return;
        }
        // in case we want a deprecated version with hw_overhead we will put config to unsupported
        // the NN model will have a unknown output and it should be thrown
        if (!results_config.is_output_supported()) {
            std::stringstream buffer;
            buffer << "Cannot load/handle Models output version. The output version: ("
                   << v.get_output_interface_version()
                   << ") is not known/supported. Version info (raw):" << v.get_raw_name();
            std::string details = buffer.str();
            Logger::error() << details;

            throw std::runtime_error(details);
        }
    }

    /**
     * @brief Ensures that input channels are equal to output channels for channel preserving operations
     *
     * @param workload a DPUWorkload that is checked and changed
     */
    void channels_preserving_operations_consistency_check(DPUWorkload& workload) const {
        if (workload.op == Operation::ELTWISE || workload.op == Operation::DW_CONVOLUTION ||
            workload.op == Operation::MAXPOOL || workload.op == Operation::AVEPOOL)
            if (workload.inputs[0].channels() != workload.outputs[0].channels()) {
                Logger::warning() << "Changed channels from " << workload.inputs[0].channels() << " to "
                                  << workload.outputs[0].channels();
                workload.inputs[0].set_shape({workload.inputs[0].x(), workload.inputs[0].y(),
                                              workload.outputs[0].channels(), workload.inputs[0].b()});
            }
    }

protected:
    /// @brief simulates AVGPOOL with another equivalent operation (DW CONV)
    ///
    /// @param workload [in, out] that will be changed in case the input is AVGPOOL
    void avgpool_replace_by(DPUWorkload& workload) const {
        if (workload.op == Operation::AVEPOOL) {
            Logger::warning() << "Workload with AVEPOOL changed to DW_CONVOLUTION";
            workload.op = Operation::DW_CONVOLUTION;
        }
    }

    /// @brief Presumes any VPU27++ CONV with IC <16 to be compressed CONV. This is known by NN as CM_CONV
    ///
    /// @param workload [in, out] that will be changed in case the input is presumed compressed CONV
    void compressConv_replace_by_CM_CONV_VPU27(DPUWorkload& workload) const {
        if (workload.device >= VPUDevice::VPU_2_7) {
            if ((workload.op == Operation::CONVOLUTION) &&
                ((workload.inputs[0].channels() > 1) && (workload.inputs[0].channels() < 16))) {
                Logger::warning() << "Workload with CONVOLUTION compressed IC[2..15] transformed to CM_CONV ";
                workload.op = Operation::CM_CONVOLUTION;
            }
        }
    }

private:
    /// @brief  check and try to make the preprocessing output to be the same as what model expects
    /// This mechanism is unsafe at this moment and lets you change the preprocessing output to a bigger or smaller size
    /// The result may be impossibility to run (if smaller) or leaving empty zeros at the end (only partial fill)
    ///
    void correlate_preprocessor_with_model_inputs() {
        const auto model_input_size = vpunn_runtime.input_tensors()[0]->shape()[1];
        const auto preprocessing_output_size = preprocessing.output_size();
        if (model_input_size != preprocessing_output_size) {
            Logger::warning() << "Changing preprocessing output size (" << preprocessing_output_size
                              << ") to the model input size (" << model_input_size << ")";
            preprocessing.set_size(model_input_size);
        }
    }

    /// 4 billion, any value higher than this might not be representable on UINT32, and
    /// should be treated like a not in range value given by the NN
    const float high_threshold{4000000000.0F};

    /// less than this is not representable on UINT32, and has no meanings in
    /// cycles. zero is still left possible to be returned, it might be a special
    /// way of network to communicate something (like no answer)
    const float low_threshold{0.0F};

    /// @brief checks if the NN returned value is invalid, is outside of usable range
    /// @param nn_output_cycles , the value to be analyzed, this is assumed given by the NN inference
    /// @return true if invalid value
    bool is_NN_value_invalid(const float nn_output_cycles) const {
        bool validity = false;
        if ((nn_output_cycles > high_threshold) || (nn_output_cycles < low_threshold)) {
            validity = true;
        }
        return validity;
    }

public:
    /// @brief provides the value interval where the NN raw outputs are considered valid and will be used to further
    /// compute information
    ///
    /// @returns a pair containing (minimum_valid_value maximum_valid_value)
    std::pair<float, float> get_NN_Valid_interval() const noexcept {
        return std::make_pair(low_threshold, high_threshold);
    }

    /**
     * @brief Construct a new VPUCostModel object
     *
     * @param filename the name of the .vpunn model
     * @param profile enable/disable profiling
     * @param cache_size the size of the LRUCache
     * @param batch_size model batch size
     */
    explicit VPUCostModel(const std::string& filename = "", bool profile = false, const unsigned int cache_size = 16384,
                          const unsigned int batch_size = 1)
            : vpunn_runtime(filename, batch_size, profile),
              preprocessing(init_preproc(preprocessing_factory, vpunn_runtime.model_version_info(), filename)),
              cache(cache_size),
              results_config(vpunn_runtime.model_version_info().get_output_interface_version()) {
        Logger::initialize();
        check_post_config(vpunn_runtime.model_version_info());

        if (!vpunn_runtime.initialized()) {
            return;
        }

        correlate_preprocessor_with_model_inputs();
        preprocessing.set_probable_batch(batch_size);
        workloads_results_buffer.reserve(prealloc_results);
    }
    // VPUCostModel(const VPUCostModel&) = delete;
    // VPUCostModel(VPUCostModel&&) = default;

    /**
     * @brief Construct a new VPUCostModel object
     *
     * @param model_data a buffer containing a .vpunn model
     * @param model_data_length the size of the model_data buffer
     * @param copy_model_data enable/disable the memcopy of the buffer
     * @param profile enable/disable profiling
     * @param cache_size the size of the LRUCache
     * @param batch_size model batch size
     */
    explicit VPUCostModel(const char* model_data, size_t model_data_length, bool copy_model_data, bool profile = false,
                          const unsigned int cache_size = 16384, const unsigned int batch_size = 1)
            : vpunn_runtime(model_data, model_data_length, copy_model_data, batch_size, profile),
              preprocessing(init_preproc(preprocessing_factory, vpunn_runtime.model_version_info(), "ConstCharInit")),
              cache(cache_size),
              results_config(vpunn_runtime.model_version_info().get_output_interface_version()) {
        Logger::initialize();
        check_post_config(vpunn_runtime.model_version_info());

        if (!vpunn_runtime.initialized()) {
            return;
        }
        correlate_preprocessor_with_model_inputs();
        preprocessing.set_probable_batch(batch_size);
        workloads_results_buffer.reserve(prealloc_results);
    }

protected:
    ///@ brief checks some validity criteria and performs sanitization that does not alter relevance
    ///
    /// @sa DPU_OperationSanitizer::check_and_sanitize for details
    /// from legacy behavior is ensures that input channels are equal to output channels for channel preserving
    /// operations
    /// @param workload [in, out] to be checked and changed
    /// @param result [out] holds error code
    /// @returns true if checks were OK, false if this wl is not to be used
    bool sanitize_workload(DPUWorkload& workload, SanityReport& result) const {
        avgpool_replace_by(workload);  // AVEPOOL will be transformed to something equivalent
        compressConv_replace_by_CM_CONV_VPU27(workload);

        channels_preserving_operations_consistency_check(workload);  // old style sanitation

        sanitizer.check_and_sanitize(workload, result);
        return result.is_usable();
    }

public:
    /**
     * @brief Compute the NN Output of a specific DPUWorkload
     * takes in consideration the cache
     * no sanitation is done
     * no check if network exists
     *
     * @param workload a DPUWorkload
     * @return float the NN raw output, not filtered
     */
    float run_NN(const DPUWorkload& workload) {
        const auto& vector = preprocessing.transform(workload);
        // Check for cache hit
        const float* const cached_value = cache.get(vector);
        if (cached_value == nullptr) {
            // run the model in case of a cache miss
            const auto infered_value = vpunn_runtime.predict<float>(vector)[0];
            // Add result to the cache
            cache.add(vector, infered_value);
            return infered_value;
        }
        return *cached_value;
    }

    /**
     * @brief Compute the NN Output of multiple DPUWorkloads
     * NOT taking in consideration the cache
     * no sanitation is done
     * no check if network exists
     *
     * @param workloads a std::vector of DPUWorkloads
     * @return a reference to the results vector. Do not store, will be invalid after next call to this method.
     */
    const std::vector<float>& run_NN(const std::vector<DPUWorkload>& workloads) {
        workloads_results_buffer.resize(workloads.size());

        if (!nn_initialized()) {  // do not run for bad NN
            std::fill(workloads_results_buffer.begin(), workloads_results_buffer.end(), default_NN_output);
            return workloads_results_buffer;
        }

        // Pre-process the workloads to generate descriptors
        const auto model_batch_size{vpunn_runtime.input_tensors()[0]->shape()[0]};  // how many wlds in a batch
        // transforms all at once, potential optimization is to do batch by batch
        const auto& vector = preprocessing.transform(workloads, model_batch_size);

        const auto descriptor_size{preprocessing.output_size()};
        const auto inputs_to_process_in_batch{descriptor_size * model_batch_size};

        for (unsigned int wl_idx = 0; wl_idx < workloads.size(); wl_idx += model_batch_size) {
            // Slice the workload descriptors and predict on a single batch
            const float* hw_overhead_arr =
                    vpunn_runtime.predict(&(vector[wl_idx * descriptor_size]), inputs_to_process_in_batch);

            const auto complete_batch_end_idx{wl_idx + model_batch_size};
            auto end_idx{(complete_batch_end_idx > workloads.size()) ? workloads.size() : complete_batch_end_idx};

            // fill into result the data for this batch
            for (unsigned int idx = wl_idx; idx < end_idx; ++idx) {
                workloads_results_buffer[idx] = hw_overhead_arr[idx - wl_idx];
            }
        }
        return workloads_results_buffer;
    }

public:
    /**
     * @brief Check if the internal VPUNN is initialized
     *
     * @return true the VPUNN neural network is initialized
     * @return false the VPUNN neural network is not initialized
     */
    bool nn_initialized() const {
        return vpunn_runtime.initialized();
    }

    /**
     * @brief Return the number of cycles needed to compute a workload
     *
     * Important: If no NN is available it will return Theoretical cycles for the workload. Check if NN is loaded with
     * nn_initialized()
     *
     * A sanity check will be performed on the workload and in case it is not suitable the method will return an error
     * code without running the inference on the NN. @sa DPU_OperationSanitizer::check_and_sanitize() explanations.
     * Some checks examples:
     * - if the device is supported
     * - if workload fits in CMX memory
     * - if the operation is supported
     *
     * List of Error codes is available in CyclesInterfaceType doc.
     *
     * A sanity check will be performed also on the NN output, in case the NN  raw value is not reliable it will not be
     * returned but an error code will be given, e.g. ERROR_INVALID_OUTPUT_RANGE
     *
     * To see the limits of valid NN values interval , use @sa get_NN_Valid_interval().  Zero is a value that will NOT
     * be filtered out.
     *
     * Behind the DPU computation is a trained Neural Network that might give unexpected results in case is asked about
     * a workload that is odd/(not well formed) or was not trained in that area or workloads.
     * The workload passed as parameter for inference should be a valid one, a one that makes sense, we are checking
     * some sanity, but ,for now, not a strict/extensive sanity check is performed. A workload with unrealistic
     * combinations of  parameters (eg DW_CONV with 7 input/output channels ) will not be detected.
     *
     * In case the wl configuration is unrealistic the network will give undefined(aberrant) results (it was not trained
     * on invalid data). The NN raw output is filtered for  generic valid interval (no negatives, no huge , e.g. 4bilion
     * cycles) but the user can also be aware of this behavior and use its own narrower ranges
     *
     * e.g.  Depending on the wl a cycle values of 10 might be unrealistic, also a value of 100milion cycles (@1Ghz is
     * ~100ms),  The user should be aware that not all aberrant/unrealistic NN outputs are handled inside.
     *
     *
     * @param wl a DPUWorkload to be evaluated.
     * @return unsigned int DPUWorkload execution cycles or an error code.
     *
     * @throws out_of_range : cache problems, cannot pre-process data , generate the NN descriptor due to data unknown
     * @throws runtime_error: cannot generate the NN descriptor, e.g expected sizes do not match
     *
     */
    /* coverity[pass_by_value] */
    CyclesInterfaceType DPU(DPUWorkload wl) {
        std::string dummy_info{};
        return DPU(wl, dummy_info);
    }

    /// @brief same like  @see DPU(DPUWorkload wl) , the extra param is to have as output the textual errors/findings
    /// discovered when handling the workload
    /// @param wl the workload to infer on
    /// @param li will collect error info regarding wl checking.
    /* coverity[pass_by_value] */
    std::tuple<CyclesInterfaceType, std::string> DPUMsg(DPUWorkload wl) {
        std::string dummy_info{};
        auto previous_print_mode{Checker::set_print_tags(false)};
        const auto r{DPU(wl, dummy_info)};
        Checker::set_print_tags(previous_print_mode);
        return std::make_tuple(r, dummy_info);
    }

    /// @brief same like  @see DPU(DPUWorkload wl) , the extra param is to have as output the textual errors/findings
    /// discovered when handling the workload
    /// @param wl the workload to infer on
    /// @param info [out] will collect error info regarding wl checking.
    /* coverity[pass_by_value] */
    CyclesInterfaceType DPU(DPUWorkload wl, std::string& info) {
        return DPU_and_sanitize(wl, info);
    }

protected:
    /* @brief DPU + wl param is also output as sanitized one.
     *  Provides workload outside so we know on what(post sanitization) was done the inference
     */
    CyclesInterfaceType DPU_and_sanitize(DPUWorkload& wl, std::string& info) {
        // sanitize and check the input.
        const auto is_inference_posible = nn_initialized();
        SanityReport problems{};
        const auto is_inference_relevant = sanitize_workload(wl, problems);
        info = problems.info;

        CyclesInterfaceType cycles{problems.value()};  // neutral value or reported problems at sanitization
        if (is_inference_relevant) {
            if (is_inference_posible) {
                const auto nn_output_cycles = run_NN(wl);
                if (is_NN_value_invalid(nn_output_cycles)) {
                    cycles = Cycles::ERROR_INVALID_OUTPUT_RANGE;
                    // std::cout << "\n Problematic inf response: "<<nn_output_cycles<<std::endl<<wl<<"\n";
                } else {
                    cycles = static_cast<CyclesInterfaceType>(ceil(nn_output_cycles));  // NORMAL CASE
                }
            } else {  // NN not available, use theoretical cycles
                cycles = DPUTheoreticalCycles(wl);
            }
        }

        return cycles;
    }

public:
    /**
     * @brief Return the number of cycles needed to compute multiple workloads
     *
     * @param workloads a std::vector of DPUWorkload
     * @return std::vector<CyclesInterfaceType> the DPUWorklaods execution cycles, @sa DPU for single wl for more
     * explanations
     */
    std::vector<CyclesInterfaceType> DPU(std::vector<DPUWorkload> workloads) {
        const auto number_of_workloads{workloads.size()};  ///< fixed value remembered here, workloads is non const
        std::vector<CyclesInterfaceType> cycles_vector = std::vector<CyclesInterfaceType>(number_of_workloads);
        const auto is_inference_posible = nn_initialized();

        /// @brief sanitization result element
        struct sanitizationOutcome {
            bool inference_relevance{false};
            SanityReport problems{};
        };
        std::vector<sanitizationOutcome> sanitization_results{number_of_workloads};

        // sanitize the input vector.
        for (unsigned int idx = 0; idx < number_of_workloads; ++idx) {
            auto& wl{workloads[idx]};
            auto& sanity{sanitization_results[idx]};
            sanity.inference_relevance = sanitize_workload(wl, sanity.problems);  // workloads are changed
        }

        // Compute using NN. Should not run if not initialized (fills a default value)
        const std::vector<float>& NN_results = run_NN(workloads);  // always tentative run

        // parse all and decide individually
        for (unsigned int idx = 0; idx < workloads.size(); ++idx) {
            auto& wl{workloads[idx]};
            const SanityReport& problems{sanitization_results[idx].problems};
            const auto is_inference_relevant{sanitization_results[idx].inference_relevance};
            const auto theoretical_cycles = DPUTheoreticalCycles(wl);

            CyclesInterfaceType cycles{problems.value()};  // neutral value or sanitization error
            if (is_inference_relevant) {
                if (is_inference_posible) {
                    const auto nn_output_cycles = NN_results[idx];
                    if (is_NN_value_invalid(nn_output_cycles)) {
                        cycles = Cycles::ERROR_INVALID_OUTPUT_RANGE;
                    } else {
                        cycles = static_cast<CyclesInterfaceType>(ceil(nn_output_cycles));  // NORMAL CASE
                    }

                } else {  // NN not available, use theoretical cycles
                    cycles = theoretical_cycles;
                }
            }

            cycles_vector[idx] = cycles;
        }

        return cycles_vector;
    }

    /**
     * @brief Compute DPUWorkload hw utilization based on ideal cycles considering also HW/sparsity.
     * This is in the context of the operation's datatype. (do not compare float with int values)
     * Represents the percentage [0,1+] of ideal resources(MAC based) used by this workload.
     * 1 = 100% of MACs are used
     * The value is calculated using the Estimated Runtime (cycles) by VPUNN.
     * If VPUNN is missing the TheoreticalCycles are used
     *
     * @param workload a DPUWorkload
     * @return  DPUWorkload hardware utilization (zero signals problems)
     */
    float hw_utilization(const DPUWorkload& wl) {
        return power_mac_hw_utilization(wl);
    }

    /**
     * @brief Compute DPUWorkload hw utilization based on ideal cycles considering also HW/sparsity.
     * This is in the context of the operation's datatype. (do not compare float with int values)
     * Represents the percentage [0,1+] of ideal resources(MAC based) used by this workload.
     * 1 = 100% of MACs are used
     * The value is calculated using the Estimated Runtime (cycles) by VPUNN.
     * If VPUNN is missing the TheoreticalCycles are used
     *
     * @param workload a DPUWorkload
     * @return  DPUWorkload hardware utilization (zero signals problems)
     */
    float power_mac_hw_utilization(const DPUWorkload& wl) {
        return mac_hw_utilization(wl, &VPUCostModel::DPU_Power_IdealCycles);
    }

    /** @bief utilization without sparsity, can be larger than one */
    float efficiency_mac_hw_utilization(const DPUWorkload& wl) {
        return mac_hw_utilization(wl, &VPUCostModel::DPU_Efficency_IdealCycles);
    }

protected:
    static_assert(std::is_same<decltype(&VPUCostModel::DPU_Efficency_IdealCycles),
                               decltype(&VPUCostModel::DPU_Power_IdealCycles)>::value,
                  "must be same signature ");

    float mac_hw_utilization(const DPUWorkload& wl,
                             decltype(&VPUCostModel::DPU_Efficency_IdealCycles) CalculateCycles) {
        std::string dummy_info{};
        DPUWorkload w{wl};
        const auto nn_output_cycles = DPU_and_sanitize(w, dummy_info);  // might change W
        const auto ideal_cycles = (this->*CalculateCycles)(w);          //< this is independent of NN cycles

        return relative_mac_hw_utilization(nn_output_cycles, ideal_cycles);
    }

    /**
     * @brief Compute DPUWorkload hw utilization based on received ideal cycles.
     * This is in the context of the operation's datatype. (do not compare float with int values)
     * Represents the percentage [0,1+] of ideal resources(MAC based) used by this workload.
     * 1 = 100% of MACs are used
     * The value is calculated using the Estimated Runtime (cycles) by VPUNN.
     * If VPUNN is missing the TheoreticalCycles are used
     * Values larger than 1 can  be obtained if the ideal_cycles are larger than eNN estimated ones
     * result = ideal_cycles/estimatedNNCycles
     *
     * @param workload a DPUWorkload
     * @param ideal_cycles the reference ideal cycles against to compute the utilization
     * @return  DPUWorkload hardware utilization (zero signals problems)
     */
    float relative_mac_hw_utilization(const CyclesInterfaceType real_cycles,
                                      const unsigned long int ideal_cycles) const {
        float utilization = 0.0F;  // zero signals problems
        const auto& nn_output_cycles = real_cycles;
        if ((!Cycles::isErrorCode(nn_output_cycles)) && nn_output_cycles != 0) {
            utilization = (float)ideal_cycles / nn_output_cycles;  // NORMAL CASE,
        }

        return utilization;
    }

public:
    /**
     * @brief Return the number of cycles needed to compute a DMA transfer
     *
     * @param device DMA VPUDevice
     * @param input DMA input Tensor
     * @param output DMA output Tensor
     * @param input_location where is the source memory
     * @param output_location where is the destination memory
     * @param output_write_tiles how many CMX tiles the DMA broadcast
     * @return unsigned int the number of cycles of the DMA transfer
     */
    unsigned int DMA(VPUDevice device, const VPUTensor& input, const VPUTensor& output,
                     MemoryLocation input_location = MemoryLocation::DRAM,
                     MemoryLocation output_location = MemoryLocation::CMX, unsigned int output_write_tiles = 1) const {
        // Call the helper function
        return DMATheoreticalCycles({device, input, output, input_location, output_location, output_write_tiles});
    }

    /**
     * @brief Return the number of cycles needed to compute a DMA transfer
     *
     * @param wl a DMAWorkload
     * @return unsigned int the number of cycles of the DMA transfer
     */
    unsigned int DMA(const DMAWorkload& wl) const {
        // Call the helper function
        return DMATheoreticalCycles(wl);
    }

    /**
     * @brief Return the number of cycles needed to compute a Shave kernel
     *
     * @param swl a Shave Kernel
     * @return unsigned int the number of cycles of the Shave kernel
     */
    unsigned int SHAVE(const SWOperation& swl) {
        return SHAVETheoreticalCycles(swl);
    }

    /**
     * @brief Return the number of cycles needed to compute a Shave kernel
     *
     * @param swl a Shave Kernel
     * @return the number of cycles of the Shave kernel, in DPU cycles. Are the DPUcycles the ones of the device (yes)?
     */
    CyclesInterfaceType SHAVE_2(const SHAVEWorkload& swl, std::string& infoOut) {
        // seek it in implemented list based on name and device
        // check if info like number of inputs & outputs and their info are OK (correlated)
        // compute the runtime.
        // infoOut += ("\n MOCK empty IMPLEMENTATION: " + swl.get_name());

        // return Cycles::ERROR_INVALID_INPUT_CONFIGURATION;

        return shave_gen_2.computeCycles(swl, infoOut);
    }
    // what would be good as meta services

    // list of supported methods
    // should it describe also each operation, more detail, like size based, wXH based?
    std::vector<std::string> getShaveSupportedOperations(VPUDevice device) const {
        // const ShaveConfiguration shaves{};
        return shave_gen_2.getShaveSupportedOperations(device);
        // std::vector<std::string> v{"empty" + VPUDevice_ToText.at(static_cast<int>(device))};
        // return v;
    };

    /**
     * @brief proxy for DPU_RelativeActivityFactor_hw
     */
    float DPUActivityFactor(const DPUWorkload& wl) {
        return DPU_PowerActivityFactor(wl);
    }

    /**
     * @brief Compute the activity factor of a DPUWorkload.
     * @details Activity factor is an estimation of the dynamic power of the DPUWorkload
     * relative to the worst case (reference dynamic power) DPUWorkload.
     * Interval [0, 1 or more], where 1 means the power virus activity factor
     * reference dynamic power is considered for INT8 operations
     * It can be more than 1 in case the PowerViruschosen for reference is not the fact the highest (like if reference
     * is power virus INT8,  the float operations can have the AF >1).
     *
     * @param wl a DPUWorkload
     * @return float the DPUWorkload activity factor relative to reference PowerVirus  (now is INT8)
     */
    float DPU_PowerActivityFactor(const DPUWorkload& wl) {
        const float mac_utilization_rate = power_mac_hw_utilization(wl);  // if zero will propagate error

        // if we have sparsity , the power per cycle might be higher (more hardware firing  for the same operation)?
        // do we need a power correction here or only at energy computation.
        // what happens when sparsity (w) is o n but we have dense values, no sparsity gain, but should be more energy
        // spent also?

        const float rough_powerVirus_relative_af = DPU_AgnosticActivityFactor(wl, mac_utilization_rate);

        const float maximum_acepted_af{power_factor_lut.get_PowerVirus_exceed_factor(wl.device)};

        const float restricted_powerVirus_relative_af = std::min(rough_powerVirus_relative_af, maximum_acepted_af);

        return restricted_powerVirus_relative_af;
    }

    float DPU_EfficiencyActivityFactor(const DPUWorkload& wl) {
        const float mac_utilization_rate = efficiency_mac_hw_utilization(wl);  // if zero will propagate error

        const float powerVirus_relative_af = DPU_AgnosticActivityFactor(wl, mac_utilization_rate);
        // no limitation  known to be applied
        return powerVirus_relative_af;
    }

protected:
    float DPU_AgnosticActivityFactor(const DPUWorkload& wl, const float reference_hw_util,
                                     const float sparse_correction_factor_experimental = 1.0F) {
        const float power_factor_value = power_factor_lut.getOperationAndPowerVirusAdjustementFactor(wl);

        return DPU_AgnosticActivityFactor_formula(power_factor_value, reference_hw_util,
                                                  sparse_correction_factor_experimental);
    }

    float DPU_AgnosticActivityFactor_formula(const float power_factor_value, const float reference_hw_util,
                                             const float sparse_correction_factor_experimental = 1.0F) {
        const float rough_powerVirus_relative_af{(reference_hw_util * power_factor_value) *
                                                 sparse_correction_factor_experimental};

        return rough_powerVirus_relative_af;
    }

public:
    /**
     * @brief Compute the energy of a DPUWorkload.
     * @details This is a relative energy metric with a time base in DPU clock cyles. Energy of
     * 1000 would mean energy of worst case power for 1000 DPU clock cycles at reference dynamic power (power virus
     * for INT08 operations). measured in PowerVirusJoules = PowerVirus*cycle
     * @param wl a DPUWorkload
     * @return float the DPUWorkload energy, measured  PowerVirus*cycle
     */
    float DPUEnergy(const DPUWorkload& wl) {
        // const float activity_factor_powerVirus = DPU_PowerActivityFactor(wl);
        // const CyclesInterfaceType cycles{DPU(wl)};
        // return calculateEnergyFromAFandTime(activity_factor_powerVirus, cycles);

        // can be further reduced to power_ideal_cycles * power_factor_value  if no limitation desired
        return calculateEnergyFromIdealCycles(wl, DPU_Power_IdealCycles(wl));
    }

protected:
    /** @brief integrates activity factor over the cycles duration=> from power to energy
     */
    float calculateEnergyFromAFandTime(const float activity_factor_powerVirus, const CyclesInterfaceType& cycles) {
        const float checked_cycles{Cycles::isErrorCode(cycles) ? 0.0F : (float)cycles};  // zero if error
        const float energy = activity_factor_powerVirus * checked_cycles;
        return energy;
    }

    float calculateEnergyFromIdealCycles(const DPUWorkload& wl, const unsigned long int reference_ideal_cycles) {
        const float power_factor_value = power_factor_lut.getOperationAndPowerVirusAdjustementFactor(wl);

        // should we scale with sparse ON but dense?
        // is there a limit, probably not as long as this is time independent

        const float energy = reference_ideal_cycles * power_factor_value;
        return energy;
    }

public:
    /**
     * @brief Compute the energy of a SHAVE SWOperation.
     * @details Energy here is a relative metric, but the activity factor of the SWOperation multiplied by
     *          its cost (number of clock cycles). We assume a constant activity factor of 0.5 for all and a max
     *          power of 5% of the DPU max power.
     *
     * @param swl a SWOperation
     * @return float the SWOperation energy , in units relative to DPU PowerVirus16
     */
    float SHAVEEnergy(const SWOperation& swl) {
        constexpr float activity_factor{0.5f};      //<assume a constant activity factor of 0.5
        const float max_power_ratio_to_DPU{0.05f};  //<assume a max power of 5% of the DPU max power.
        const float energy = (activity_factor * max_power_ratio_to_DPU) * (float)SHAVE(swl);

        return energy;
    }

    /// @brief same like  @see DPU(DPUWorkload wl) but return a Pack of information regarding the workload
    /// The purpose of this Method is to replace several separate calls to individual informations about the same
    /// workload.
    /// For example , estimated  cycle-times, errors, energy, activity factor, all can be obtained in one call.
    /// This method has the potential to be more efficient that the collection of individual ones.
    /// @param wl the workload to infer on
    /// @returns a Structure with all info that L1 APi can provide about this Workload
    DPUInfoPack DPUInfo(const DPUWorkload& workload) {
        DPUInfoPack allData;      // expect RVO when returning it!
        DPUWorkload w{workload};  // local clone

        allData.DPUCycles = DPU_and_sanitize(w, allData.errInfo);  // do this first, might change w

        {
            allData.sparse_mac_operations = compute_HW_MAC_operations_cnt(w);
            allData.power_ideal_cycles = DPU_Power_IdealCycles(w);
            allData.power_mac_utilization = relative_mac_hw_utilization(allData.DPUCycles, allData.power_ideal_cycles);
            // to be restricted
            {
                const float rough_powerVirus_relative_af =
                        DPU_AgnosticActivityFactor(w, allData.power_mac_utilization);  // DPU_PowerActivityFactor(w);

                const float nominal_allowed_Virus_exceed_factor{
                        power_factor_lut.get_PowerVirus_exceed_factor(w.device)};
                const float restricted_powerVirus_relative_af =
                        std::min(rough_powerVirus_relative_af, nominal_allowed_Virus_exceed_factor);
                allData.power_activity_factor = restricted_powerVirus_relative_af;
            }

            // allData.energy = calculateEnergyFromAFandTime(allData.power_activity_factor, allData.DPUCycles);
            allData.energy = calculateEnergyFromIdealCycles(w, allData.power_ideal_cycles);
        }

        {
            allData.dense_mac_operations = compute_Ideal_MAC_operations_cnt(w);
            allData.efficiency_ideal_cycles = DPU_Efficency_IdealCycles(w);
            allData.efficiency_mac_utilization =
                    relative_mac_hw_utilization(allData.DPUCycles, allData.efficiency_ideal_cycles);
            allData.efficiency_activity_factor = DPU_AgnosticActivityFactor(
                    w, allData.efficiency_mac_utilization);  // DPU_EfficiencyActivityFactor(w);
        }

        allData.hw_theoretical_cycles = DPUTheoreticalCycles(w);

        return allData;  // rvo
    }
};
}  // namespace VPUNN

#endif  // VPU_COST_MODEL_H
