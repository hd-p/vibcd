// Single include point for every Rockchip MPI / RTSP header.
//
// The C headers are not uniformly wrapped in `extern "C"`, and the previous
// version of this project had each translation unit include an ad-hoc subset,
// which is how calls to RK_MPI_MB_* ended up unresolved in one file while
// compiling fine in another. Everything funnels through here instead.

#ifndef BABY_MONITOR_BASE_RK_PLATFORM_H
#define BABY_MONITOR_BASE_RK_PLATFORM_H

extern "C" {

#include "rk_comm_aenc.h"
#include "rk_comm_aio.h"
#include "rk_comm_ivs.h"
#include "rk_comm_mb.h"
#include "rk_comm_venc.h"
#include "rk_comm_vi.h"
#include "rk_comm_video.h"
#include "rk_comm_vpss.h"
#include "rk_common.h"
#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_ivs.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vpss.h"

#include "rtsp_demo.h"

}  // extern "C"

#endif  // BABY_MONITOR_BASE_RK_PLATFORM_H
