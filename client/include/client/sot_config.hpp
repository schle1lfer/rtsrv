/**
 * @file client/include/client/sot_config.hpp
 * @brief Re-exports the SOT configuration types from common for sra.
 *
 * The canonical parsing implementation now lives in common/sot_config.hpp so
 * that it can be shared with srmd.  This header re-exports every type and the
 * loader function into the @c sra namespace so that all existing sra call-
 * sites compile without modification.
 *
 * @version 1.0
 */

#pragma once

#include "common/sot_config.hpp"

namespace sra
{

using common::SotPrefix;
using common::SotInterface;
using common::SotVrfIpv4;
using common::SotVrf;
using common::SotLoopbacks;
using common::SotNode;
using common::SotConfig;
using common::loadSotConfig;

} // namespace sra
