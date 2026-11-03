#!/bin/bash
#
# Launch batch_host (xrt_batches: CREATE once / RUN batches / DESTROY) from the
# x86 host. RPU_APP must be xrt_batches.elf, freshly loaded for every run.
#
# Usage: ./launch-batches.sh [resnet50-int8|resnet50-nopool|bevformer|simple_int8] [extra batch_host args]
#   e.g. ./launch-batches.sh resnet50-int8 --batches 10 --verbose
#
# Kernel args are given in kernel-arg order (index = position). --shared-in is
# sent once at CREATE; --in / --in-0 / --out are per entry. One RUN request must
# fit 60 args: 4 + K + 2*N*K, so N <= 8 entries for K = 3, N <= 6 for K = 4.

APPDIR=/home/amd/wendy/amdrnpu/tests/rpmsg_dma_xrt_offload
APP=${APPDIR}/host/batch_host
HOSTDIR=${APPDIR}/host

MODEL=${1:-resnet50-int8}
shift

case "${MODEL}" in
resnet50-int8)
	MODDIR=${HOSTDIR}/resnet50-int8-t20
	IFM=${MODDIR}/inputSubBO_vaiml_par_0_id_0_0_0_trim.bin
	${APP} --ini ${APPDIR}/offload_config.ini --entries 3 --batches 100 \
	  --model     ${MODDIR}/aie_control-4cols.elf \
	  --shared-in ${MODDIR}/wts32-4cols.bin \
	  --in        ${IFM},${IFM},${IFM} \
	  --in-0      2408448 \
	  --out       1000 \
	  "$@"
	;;
resnet50-nopool)
	MODDIR=${HOSTDIR}/resnet50-nopool-t20
	${APP} --ini ${APPDIR}/offload_config.ini --entries 3 --batches 100 \
	  --model     ${MODDIR}/aie_control.elf \
	  --shared-in ${MODDIR}/wts32.bin \
	  --in        ${MODDIR}/ifm_dog.npy \
	  --in-0      1634304 \
	  --out       229376 \
	  "$@"
	;;
bevformer)
	MODDIR=${HOSTDIR}/bevformer-t20
	${APP} --ini ${APPDIR}/offload_config.ini --entries 3 --batches 100 \
	  --model     ${MODDIR}/aie_control_james.elf \
	  --shared-in ${MODDIR}/wts32.bin \
	  --in        ${MODDIR}/inputSubBO_vaiml_par_0_id_0_0_0_trim.bin \
	  --in-0      4403200 \
	  --out       18000 \
	  --out       18000 \
	  "$@"
	;;
simple_int8)
	MODDIR=${HOSTDIR}/simple_int8
	${APP} --ini ${APPDIR}/offload_config.ini --entries 3 --batches 1 \
	  --model     ${MODDIR}/aie_control.elf \
	  --in        ${MODDIR}/ifm32.bin \
	  --in-0      1536 \
	  --shared-in ${MODDIR}/wts32.bin \
	  "$@"
	;;
*)
	echo "unknown model '${MODEL}' (resnet50-int8|resnet50-nopool|bevformer|simple_int8)" >&2
	exit 2
	;;
esac
