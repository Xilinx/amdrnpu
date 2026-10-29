#!/bin/bash
#
# Launch sensor_pipeline on the rave2 x86 host: 3 sensors on the T20 RPU
# (RPU_APP = xrt_batches.elf, freshly loaded for every run) + 3 sensors on the
# local Strix-class NPU, ResNet50-int8, ping-pong buffers.
#
# Usage: ./launch-sensor-pipeline.sh [orig|feat|fusion] [extra sensor_pipeline args]
#   orig (default)  ResNet50 classifier: OFM = 1000 int8 logits per sensor
#   feat            ResNet50 cut after layer4 (no avgpool/FC): OFM = [2048,7,7]
#                   int8 feature map per sensor (scale 0.125) = 49 location
#                   tokens x 2048 for a downstream VLM/VLA
#   fusion          feat + final step = fusion6 on the local NPU: all 6 sensor
#                   OFMs (re-laid out on the host) -> [2560,42,7] int8 (scale
#                   1/32) = 294 tokens x 2560; frame 0's output is saved to
#                   out_fusion/fusion_ofm.bin
#   MODEL=feat ./launch-sensor-pipeline.sh  also works.
#
#   e.g. ./launch-sensor-pipeline.sh --frames 1000 --post-us 5000
#        ./launch-sensor-pipeline.sh feat --frames 1000
#        ./launch-sensor-pipeline.sh fusion --frames 1000
#        ./launch-sensor-pipeline.sh --slots 1          # no overlap, for comparison
#        ./launch-sensor-pipeline.sh --no-rpu           # local NPU only
#        ./launch-sensor-pipeline.sh --no-strix         # T20 RPU only
#        ./launch-sensor-pipeline.sh --lockstep         # also start both batches together
#
# One batch per side per frame = 3 sensors, each with its own IFM, spill and OFM
# buffer (per slot). --*-ifm / --*-golden take one file per sensor (or one file
# for all).
#
# orig: only one CI image + golden exists for the Strix model, so the 3 Strix
#       sensors reuse it.
# feat: 6 distinct sensor views io/ifm{0..5}_hw.bin (same int8 HCWNC4 input on
#       both targets): T20 runs ifm0..2, Strix runs ifm3..5. Strix goldens are
#       exact NPU outputs captured on a Strix NPU (xsjstrix44). There is no T20
#       golden yet: frame 0's OFMs are written to out_feat/ for an offline check
#       (resnet50-int8-feat-t20/check_ofm.py); once verified, rpu_s*.bin can be
#       passed as --rpu-golden.
#
# Later options override earlier ones, so extra args win over the defaults.

APPDIR=/home/amd/wendy/amdrnpu/tests/rpmsg_dma_xrt_offload
HOSTDIR=${APPDIR}/host
APP=${HOSTDIR}/sensor_pipeline

MODEL=${MODEL:-orig}
case "${1:-}" in
orig|feat|fusion) MODEL=$1; shift ;;
esac

case "${MODEL}" in
orig)
	RPU=${HOSTDIR}/resnet50-int8-t20
	STRIX=${HOSTDIR}/resnet50-int8-strix
	set -- \
	  --rpu-elf    ${RPU}/aie_control-4cols.elf \
	  --rpu-wts    ${RPU}/wts32-4cols.bin \
	  --rpu-ifm    ${RPU}/inputSubBO_vaiml_par_0_id_0_0_0_trim.bin,${RPU}/inputSubBO_vaiml_par_0_id_0_0_1_trim.bin,${RPU}/inputSubBO_vaiml_par_0_id_0_0_2_trim.bin \
	  --rpu-spill  2408448 \
	  --rpu-ofm    1000 \
	  --rpu-args   0,1,2,3 \
	  --strix-dir  ${STRIX} \
	  --strix-ifm  ${STRIX}/ifm_hw.bin,${STRIX}/ifm_hw.bin,${STRIX}/ifm_hw.bin \
	  --strix-golden ${STRIX}/ofm_golden.bin,${STRIX}/ofm_golden.bin,${STRIX}/ofm_golden.bin \
	  "$@"
	;;
feat|fusion)
	RPU=${HOSTDIR}/resnet50-int8-feat-t20
	STRIX=${HOSTDIR}/resnet50-int8-feat-strix
	FUSION=${HOSTDIR}/fusion6-strix
	OUT=${HOSTDIR}/out_${MODEL}
	mkdir -p ${OUT}
	if [ "${MODEL}" = fusion ]; then
		# fusion input k = sensor k (0..2 T20, 3..5 Strix); arg indices, sizes
		# and IFM layout from fusion6's external_buffer_id.json / flexmlrt-hsi.json
		set -- \
		  --fusion-dir        ${FUSION} \
		  --fusion-ifm-args   3,8,4,7,6,5 \
		  --fusion-args       9,10,11,12 \
		  --fusion-sizes      114688,12472320,1376256,752640 \
		  --rpu-ofm-layout    HCWNC8:7,256,7,1,8 \
		  --strix-ofm-layout  HCWNC8:8,256,7,1,8 \
		  --fusion-ifm-layout NCHW:1,2048,7,8 \
		  "$@"
	fi
	set -- \
	  --rpu-elf    ${RPU}/aie_control.elf \
	  --rpu-wts    ${RPU}/wts32.bin \
	  --rpu-ifm    ${RPU}/io/ifm0_hw.bin,${RPU}/io/ifm1_hw.bin,${RPU}/io/ifm2_hw.bin \
	  --rpu-spill  1605632 \
	  --rpu-ofm    100352 \
	  --rpu-args   0,1,2,3 \
	  --strix-dir  ${STRIX} \
	  --strix-ifm  ${RPU}/io/ifm3_hw.bin,${RPU}/io/ifm4_hw.bin,${RPU}/io/ifm5_hw.bin \
	  --strix-sizes 200704,24428544,301056,114688 \
	  --strix-args  3,4,5,6,7 \
	  --strix-golden ${STRIX}/golden/ofm_batch3.bin,${STRIX}/golden/ofm_batch4.bin,${STRIX}/golden/ofm_batch5.bin \
	  --save-out   ${OUT} \
	  "$@"
	;;
*)
	echo "unknown MODEL '${MODEL}' (use orig, feat or fusion)" >&2
	exit 1
	;;
esac

echo "== MODEL=${MODEL}"
${APP} --ini ${APPDIR}/offload_config.ini --sensors 3 --frames 100 --slots 2 "$@"
