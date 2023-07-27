// Copyright © 2023 Intel Corporation
// SPDX-License-Identifier: Apache 2.0
// LEGAL NOTICE: Your use of this software and any required dependent software (the “Software Package”)
// is subject to the terms and conditions of the software license agreements for the Software Package,
// which may also include notices, disclaimers, or license terms for third party or open source software
// included in or with the Software Package, and your use indicates your acceptance of all such terms.
// Please refer to the “third-party-programs.txt” or other similarly-named text file included with the
// Software Package for additional details.

#ifndef PREPROC_FACTORY_H
#define PREPROC_FACTORY_H

#include <math.h>
#include <vpu/compatibility/types01.h>  // detailed implementations
#include <vpu/compatibility/types11.h>  // detailed implementations
#include <vpu/types.h>
#include <sstream>  // for error formating
#include <stdexcept>
#include <string>
#include <vector>
#include "preprocessing.h"

namespace VPUNN {
/**
 * @brief Provides processing related objects based on context
 *
 * The provided objects may be bounded(lifespan) to this instance
 */
class RuntimeProcessingFactory {
private:
    using PrepropType = Preprocessing<float>;
    using PreprocessingMap = std::map<int, PrepropType&>;

    // a simple and not optimum (allocates all static)
    PreprocessingLatest<float> pp_v00_latest;
    Preprocessing_Interface01<float> pp_v01_base;
    Preprocessing_Interface10<float> pp_v10;
    Preprocessing_Interface11<float> pp_v11;

    /// @brief the map of versions mapped to preprocessing concrete objects
    const PreprocessingMap pp_map{
            {pp_v00_latest.getInterfaceVersion(), pp_v00_latest},
            {pp_v01_base.getInterfaceVersion(), pp_v01_base},
            {pp_v10.getInterfaceVersion(), pp_v10},
            {pp_v11.getInterfaceVersion(), pp_v11},
    };

public:
    /// @brief True if a preprocessor exists for required/interrogated version
    bool exists_preprocessing(int input_version) const noexcept {
        auto found = pp_map.find(input_version);
        return (found != pp_map.cend());
    }
    /** @brief provides a preprocessor for the required interface
     * The provided preprocessor is owned by this class.
     * For NOW multiple requests for the same version will provide the same object, the factory just shares the
     * preprocessors , does not create a new one for each request
     * @param version desired interface version
     * @return the preprocessor object to be used (shared)
     * @throws out_of_range in case the version is not supported
     */
    Preprocessing<float>& make_preprocessing(int version) const {
        if (exists_preprocessing(version)) {
            return pp_map.at(version);
        }

        // throw
        std::stringstream buffer;
        buffer << "[ERROR]:Preprocessing cannot be created for version:   " << version;
        std::string details = buffer.str();
        throw std::out_of_range(details);
    }
};

}  // namespace VPUNN
#endif  // guard
