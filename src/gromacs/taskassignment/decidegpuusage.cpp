/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2015,2016,2017,2018,2019, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 * \brief Defines functionality for deciding whether tasks will run on GPUs.
 *
 * \author Mark Abraham <mark.j.abraham@gmail.com>
 * \ingroup module_taskassignment
 */

#include "gmxpre.h"

#include "decidegpuusage.h"

#include "config.h"

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <string>

#include "gromacs/ewald/pme.h"
#include "gromacs/hardware/cpuinfo.h"
#include "gromacs/hardware/detecthardware.h"
#include "gromacs/hardware/hardwaretopology.h"
#include "gromacs/hardware/hw_info.h"
#include "gromacs/mdlib/gmx_omp_nthreads.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/mdrunoptions.h"
#include "gromacs/taskassignment/taskassignment.h"
#include "gromacs/topology/topology.h"
#include "gromacs/utility/baseversion.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/logger.h"
#include "gromacs/utility/stringutil.h"


namespace gmx
{

namespace
{

//! Helper variable to localise the text of an often repeated message.
const char* g_specifyEverythingFormatString =
        "When you use mdrun -gputasks, %s must be set to non-default "
        "values, so that the device IDs can be interpreted correctly."
#if GMX_GPU != GMX_GPU_NONE
        " If you simply want to restrict which GPUs are used, then it is "
        "better to use mdrun -gpu_id. Otherwise, setting the "
#    if GMX_GPU == GMX_GPU_CUDA
        "CUDA_VISIBLE_DEVICES"
#    elif GMX_GPU == GMX_GPU_OPENCL
        // Technically there is no portable way to do this offered by the
        // OpenCL standard, but the only current relevant case for GROMACS
        // is AMD OpenCL, which offers this variable.
        "GPU_DEVICE_ORDINAL"
#    else
#        error "Unreachable branch"
#    endif
        " environment variable in your bash profile or job "
        "script may be more convenient."
#endif
        ;

} // namespace

bool decideWhetherToUseGpusForNonbondedWithThreadMpi(const TaskTarget        nonbondedTarget,
                                                     const std::vector<int>& gpuIdsToUse,
                                                     const std::vector<int>& userGpuTaskAssignment,
                                                     const EmulateGpuNonbonded emulateGpuNonbonded,
                                                     const bool buildSupportsNonbondedOnGpu,
                                                     const bool nonbondedOnGpuIsUseful,
                                                     const int  numRanksPerSimulation)
{
    // First, exclude all cases where we can't run NB on GPUs.
    if (nonbondedTarget == TaskTarget::Cpu || emulateGpuNonbonded == EmulateGpuNonbonded::Yes
        || !nonbondedOnGpuIsUseful || !buildSupportsNonbondedOnGpu)
    {
        // If the user required NB on GPUs, we issue an error later.
        return false;
    }

    // We now know that NB on GPUs makes sense, if we have any.

    if (!userGpuTaskAssignment.empty())
    {
        // Specifying -gputasks requires specifying everything.
        if (nonbondedTarget == TaskTarget::Auto || numRanksPerSimulation < 1)
        {
            GMX_THROW(InconsistentInputError(
                    formatString(g_specifyEverythingFormatString, "-nb and -ntmpi")));
        }
        return true;
    }

    if (nonbondedTarget == TaskTarget::Gpu)
    {
        return true;
    }

    // Because this is thread-MPI, we already know about the GPUs that
    // all potential ranks can use, and can use that in a global
    // decision that will later be consistent.
    auto haveGpus = !gpuIdsToUse.empty();

    // If we get here, then the user permitted or required GPUs.
    return haveGpus;
}

bool decideWhetherToUseGpusForPmeWithThreadMpi(const bool              useGpuForNonbonded,
                                               const TaskTarget        pmeTarget,
                                               const std::vector<int>& gpuIdsToUse,
                                               const std::vector<int>& userGpuTaskAssignment,
                                               const gmx_hw_info_t&    hardwareInfo,
                                               const t_inputrec&       inputrec,
                                               const gmx_mtop_t&       mtop,
                                               const int               numRanksPerSimulation,
                                               const int               numPmeRanksPerSimulation)
{
    // First, exclude all cases where we can't run PME on GPUs.
    if ((pmeTarget == TaskTarget::Cpu) || !useGpuForNonbonded || !pme_gpu_supports_build(nullptr)
        || !pme_gpu_supports_hardware(hardwareInfo, nullptr)
        || !pme_gpu_supports_input(inputrec, mtop, nullptr))
    {
        // PME can't run on a GPU. If the user required that, we issue
        // an error later.
        return false;
    }

    // We now know that PME on GPUs might make sense, if we have any.

    if (!userGpuTaskAssignment.empty())
    {
        // Follow the user's choice of GPU task assignment, if we
        // can. Checking that their IDs are for compatible GPUs comes
        // later.

        // Specifying -gputasks requires specifying everything.
        if (pmeTarget == TaskTarget::Auto || numRanksPerSimulation < 1)
        {
            GMX_THROW(InconsistentInputError(
                    formatString(g_specifyEverythingFormatString, "all of -nb, -pme, and -ntmpi")));
        }

        // PME on GPUs is only supported in a single case
        if (pmeTarget == TaskTarget::Gpu)
        {
            if (((numRanksPerSimulation > 1) && (numPmeRanksPerSimulation == 0))
                || (numPmeRanksPerSimulation > 1))
            {
                GMX_THROW(InconsistentInputError(
                        "When you run mdrun -pme gpu -gputasks, you must supply a PME-enabled .tpr "
                        "file and use a single PME rank."));
            }
            return true;
        }

        // pmeTarget == TaskTarget::Auto
        return numRanksPerSimulation == 1;
    }

    // Because this is thread-MPI, we already know about the GPUs that
    // all potential ranks can use, and can use that in a global
    // decision that will later be consistent.

    if (pmeTarget == TaskTarget::Gpu)
    {
        if (((numRanksPerSimulation > 1) && (numPmeRanksPerSimulation == 0))
            || (numPmeRanksPerSimulation > 1))
        {
            GMX_THROW(NotImplementedError(
                    "PME tasks were required to run on GPUs, but that is not implemented with "
                    "more than one PME rank. Use a single rank simulation, or a separate PME rank, "
                    "or permit PME tasks to be assigned to the CPU."));
        }
        return true;
    }

    if (numRanksPerSimulation == 1)
    {
        // PME can run well on a GPU shared with NB, and we permit
        // mdrun to default to try that.
        return !gpuIdsToUse.empty();
    }

    if (numRanksPerSimulation < 1)
    {
        // Full automated mode for thread-MPI (the default). PME can
        // run well on a GPU shared with NB, and we permit mdrun to
        // default to it if there is only one GPU available.
        return (gpuIdsToUse.size() == 1);
    }

    // Not enough support for PME on GPUs for anything else
    return false;
}

bool decideWhetherToUseGpusForNonbonded(const TaskTarget          nonbondedTarget,
                                        const std::vector<int>&   userGpuTaskAssignment,
                                        const EmulateGpuNonbonded emulateGpuNonbonded,
                                        const bool                buildSupportsNonbondedOnGpu,
                                        const bool                nonbondedOnGpuIsUseful,
                                        const bool                gpusWereDetected)
{
    if (nonbondedTarget == TaskTarget::Cpu)
    {
        if (!userGpuTaskAssignment.empty())
        {
            GMX_THROW(InconsistentInputError(
                    "A GPU task assignment was specified, but nonbonded interactions were "
                    "assigned to the CPU. Make no more than one of these choices."));
        }

        return false;
    }

    if (!buildSupportsNonbondedOnGpu && nonbondedTarget == TaskTarget::Gpu)
    {
        GMX_THROW(InconsistentInputError(
                "Nonbonded interactions on the GPU were requested with -nb gpu, "
                "but the GROMACS binary has been built without GPU support. "
                "Either run without selecting GPU options, or recompile GROMACS "
                "with GPU support enabled"));
    }

    // TODO refactor all these TaskTarget::Gpu checks into one place?
    // e.g. use a subfunction that handles only the cases where
    // TaskTargets are not Cpu?
    if (emulateGpuNonbonded == EmulateGpuNonbonded::Yes)
    {
        if (nonbondedTarget == TaskTarget::Gpu)
        {
            GMX_THROW(InconsistentInputError(
                    "Nonbonded interactions on the GPU were required, which is inconsistent "
                    "with choosing emulation. Make no more than one of these choices."));
        }
        if (!userGpuTaskAssignment.empty())
        {
            GMX_THROW(
                    InconsistentInputError("GPU ID usage was specified, as was GPU emulation. Make "
                                           "no more than one of these choices."));
        }

        return false;
    }

    if (!nonbondedOnGpuIsUseful)
    {
        if (nonbondedTarget == TaskTarget::Gpu)
        {
            GMX_THROW(InconsistentInputError(
                    "Nonbonded interactions on the GPU were required, but not supported for these "
                    "simulation settings. Change your settings, or do not require using GPUs."));
        }

        return false;
    }

    if (!userGpuTaskAssignment.empty())
    {
        // Specifying -gputasks requires specifying everything.
        if (nonbondedTarget == TaskTarget::Auto)
        {
            GMX_THROW(InconsistentInputError(
                    formatString(g_specifyEverythingFormatString, "-nb and -ntmpi")));
        }

        return true;
    }

    if (nonbondedTarget == TaskTarget::Gpu)
    {
        // We still don't know whether it is an error if no GPUs are found
        // because we don't know the duty of this rank, yet. For example,
        // a node with only PME ranks and -pme cpu is OK if there are not
        // GPUs.
        return true;
    }

    // If we get here, then the user permitted GPUs, which we should
    // use for nonbonded interactions.
    return gpusWereDetected;
}

bool decideWhetherToUseGpusForPme(const bool              useGpuForNonbonded,
                                  const TaskTarget        pmeTarget,
                                  const std::vector<int>& userGpuTaskAssignment,
                                  const gmx_hw_info_t&    hardwareInfo,
                                  const t_inputrec&       inputrec,
                                  const gmx_mtop_t&       mtop,
                                  const int               numRanksPerSimulation,
                                  const int               numPmeRanksPerSimulation,
                                  const bool              gpusWereDetected)
{
    if (pmeTarget == TaskTarget::Cpu)
    {
        return false;
    }

    if (!useGpuForNonbonded)
    {
        if (pmeTarget == TaskTarget::Gpu)
        {
            GMX_THROW(NotImplementedError(
                    "PME on GPUs is only supported when nonbonded interactions run on GPUs also."));
        }
        return false;
    }

    std::string message;
    if (!pme_gpu_supports_build(&message))
    {
        if (pmeTarget == TaskTarget::Gpu)
        {
            GMX_THROW(NotImplementedError("Cannot compute PME interactions on a GPU, because " + message));
        }
        return false;
    }
    if (!pme_gpu_supports_hardware(hardwareInfo, &message))
    {
        if (pmeTarget == TaskTarget::Gpu)
        {
            GMX_THROW(NotImplementedError("Cannot compute PME interactions on a GPU, because " + message));
        }
        return false;
    }
    if (!pme_gpu_supports_input(inputrec, mtop, &message))
    {
        if (pmeTarget == TaskTarget::Gpu)
        {
            GMX_THROW(NotImplementedError("Cannot compute PME interactions on a GPU, because " + message));
        }
        return false;
    }

    if (pmeTarget == TaskTarget::Cpu)
    {
        if (!userGpuTaskAssignment.empty())
        {
            GMX_THROW(InconsistentInputError(
                    "A GPU task assignment was specified, but PME interactions were "
                    "assigned to the CPU. Make no more than one of these choices."));
        }

        return false;
    }

    if (!userGpuTaskAssignment.empty())
    {
        // Specifying -gputasks requires specifying everything.
        if (pmeTarget == TaskTarget::Auto)
        {
            GMX_THROW(InconsistentInputError(formatString(
                    g_specifyEverythingFormatString, "all of -nb, -pme, and -ntmpi"))); // TODO ntmpi?
        }

        return true;
    }

    // We still don't know whether it is an error if no GPUs are found
    // because we don't know the duty of this rank, yet. For example,
    // a node with only PME ranks and -pme cpu is OK if there are not
    // GPUs.

    if (pmeTarget == TaskTarget::Gpu)
    {
        if (((numRanksPerSimulation > 1) && (numPmeRanksPerSimulation == 0))
            || (numPmeRanksPerSimulation > 1))
        {
            GMX_THROW(NotImplementedError(
                    "PME tasks were required to run on GPUs, but that is not implemented with "
                    "more than one PME rank. Use a single rank simulation, or a separate PME rank, "
                    "or permit PME tasks to be assigned to the CPU."));
        }
        return true;
    }

    // If we get here, then the user permitted GPUs.
    if (numRanksPerSimulation == 1)
    {
        // PME can run well on a single GPU shared with NB when there
        // is one rank, so we permit mdrun to try that if we have
        // detected GPUs.
        return gpusWereDetected;
    }

    // Not enough support for PME on GPUs for anything else
    return false;
}

bool decideWhetherToUseGpusForBonded(const bool       useGpuForNonbonded,
                                     const bool       useGpuForPme,
                                     const TaskTarget bondedTarget,
                                     const bool       canUseGpuForBonded,
                                     const bool       usingLJPme,
                                     const bool       usingElecPmeOrEwald,
                                     const int        numPmeRanksPerSimulation,
                                     const bool       gpusWereDetected)
{
    if (bondedTarget == TaskTarget::Cpu)
    {
        return false;
    }

    if (!canUseGpuForBonded)
    {
        if (bondedTarget == TaskTarget::Gpu)
        {
            GMX_THROW(InconsistentInputError(
                    "Bonded interactions on the GPU were required, but not supported for these "
                    "simulation settings. Change your settings, or do not require using GPUs."));
        }

        return false;
    }

    if (!useGpuForNonbonded)
    {
        if (bondedTarget == TaskTarget::Gpu)
        {
            GMX_THROW(InconsistentInputError(
                    "Bonded interactions on the GPU were required, but this requires that "
                    "short-ranged non-bonded interactions are also run on the GPU. Change "
                    "your settings, or do not require using GPUs."));
        }

        return false;
    }

    // TODO If the bonded kernels do not get fused, then performance
    // overheads might suggest alternative choices here.

    if (bondedTarget == TaskTarget::Gpu)
    {
        // We still don't know whether it is an error if no GPUs are
        // found.
        return true;
    }

    // If we get here, then the user permitted GPUs, which we should
    // use for bonded interactions if any were detected and the CPU
    // is busy, for which we currently only check PME or Ewald.
    // (It would be better to dynamically assign bondeds based on timings)
    // Note that here we assume that the auto setting of PME ranks will not
    // choose seperate PME ranks when nonBonded are assigned to the GPU.
    bool usingOurCpuForPmeOrEwald =
            (usingLJPme || (usingElecPmeOrEwald && !useGpuForPme && numPmeRanksPerSimulation <= 0));

    return gpusWereDetected && usingOurCpuForPmeOrEwald;
}

bool decideWhetherToUseGpuForUpdate(const bool        forceGpuUpdateDefaultOn,
                                    const bool        isDomainDecomposition,
                                    const bool        useGpuForPme,
                                    const bool        useGpuForNonbonded,
                                    const TaskTarget  updateTarget,
                                    const bool        gpusWereDetected,
                                    const t_inputrec& inputrec,
                                    const bool        haveVSites,
                                    const bool        useEssentialDynamics,
                                    const bool        doOrientationRestraints,
                                    const bool        useReplicaExchange)
{

    if (updateTarget == TaskTarget::Cpu)
    {
        return false;
    }

    std::string errorMessage;

    if (isDomainDecomposition)
    {
        errorMessage += "Domain decomposition is not supported.\n";
    }
    // Using the GPU-version of update if:
    // 1. PME is on the GPU (there should be a copy of coordinates on GPU for PME spread), or
    // 2. Non-bonded interactions are on the GPU.
    if (!(useGpuForPme || useGpuForNonbonded))
    {
        errorMessage +=
                "Either PME or short-ranged non-bonded interaction tasks must run on the GPU.\n";
    }
    if (!gpusWereDetected)
    {
        errorMessage += "Compatible GPUs must have been found.\n";
    }
    if (GMX_GPU != GMX_GPU_CUDA)
    {
        errorMessage += "Only a CUDA build is supported.\n";
    }
    if (inputrec.eI != eiMD)
    {
        errorMessage += "Only the md integrator is supported.\n";
    }
    if (inputrec.etc == etcNOSEHOOVER)
    {
        errorMessage += "Nose-Hoover temperature coupling is not supported.\n";
    }
    if (!(inputrec.epc == epcNO || inputrec.epc == epcPARRINELLORAHMAN || inputrec.epc == epcBERENDSEN))
    {
        errorMessage += "Only Parrinello-Rahman and Berendsen pressure coupling are supported.\n";
    }
    if (EEL_PME_EWALD(inputrec.coulombtype) && inputrec.epsilon_surface != 0)
    {
        // The graph is needed, but not supported
        errorMessage += "Ewald surface correction is not supported.\n";
    }
    if (haveVSites)
    {
        errorMessage += "Virtual sites are not supported.\n";
    }
    if (useEssentialDynamics)
    {
        errorMessage += "Essential dynamics is not supported.\n";
    }
    if (inputrec.bPull || inputrec.pull)
    {
        // Pull potentials are actually supported, but constraint pulling is not
        errorMessage += "Pulling is not supported.\n";
    }
    if (doOrientationRestraints)
    {
        // The graph is needed, but not supported
        errorMessage += "Orientation restraints are not supported.\n";
    }
    if (inputrec.efep != efepNO)
    {
        // Actually all free-energy options except for mass and constraint perturbation are supported
        errorMessage += "Free energy perturbations are not supported.\n";
    }
    if (useReplicaExchange)
    {
        errorMessage += "Replica exchange simulations are not supported.\n";
    }
    if (inputrec.eSwapCoords != eswapNO)
    {
        errorMessage += "Swapping the coordinates is not supported.\n";
    }

    // \todo Check for coupled constraint block size restriction needs to be added
    //       when update auto chooses GPU in some cases. Currently exceeding the restriction
    //       triggers a fatal error during LINCS setup.

    if (!errorMessage.empty())
    {
        if (updateTarget == TaskTarget::Gpu)
        {
            std::string prefix = gmx::formatString(
                    "Update task on the GPU was required,\n"
                    "but the following condition(s) were not satisfied:\n");
            GMX_THROW(InconsistentInputError((prefix + errorMessage).c_str()));
        }
        return false;
    }

    return ((forceGpuUpdateDefaultOn && updateTarget == TaskTarget::Auto)
            || (updateTarget == TaskTarget::Gpu));
}

} // namespace gmx
