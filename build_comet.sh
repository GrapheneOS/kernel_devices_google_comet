#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=comet \
    --config=fast \
    --config=pixel_debug_common \
    --config=aosp_staging \
    //private/devices/google/comet:zumapro_comet_dist "$@"
