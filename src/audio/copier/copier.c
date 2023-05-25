// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
// Author: Rander Wang <rander.wang@linux.intel.com>

#include <sof/audio/buffer.h>
#include <sof/audio/component_ext.h>
#include <sof/audio/format.h>
#include <sof/audio/pipeline.h>
#include <sof/common.h>
#include <rtos/panic.h>
#include <rtos/interrupt.h>
#include <sof/ipc/msg.h>
#include <sof/ipc/topology.h>
#include <rtos/interrupt.h>
#include <rtos/timer.h>
#include <rtos/alloc.h>
#include <rtos/cache.h>
#include <rtos/init.h>
#include <sof/lib/memory.h>
#include <sof/lib/uuid.h>
#include <sof/list.h>
#include <rtos/string.h>
#include <sof/ut.h>
#include <sof/trace/trace.h>
#include <ipc4/alh.h>
#include <ipc4/copier.h>
#include <ipc4/module.h>
#include <ipc4/error_status.h>
#include <ipc4/gateway.h>
#include <ipc4/fw_reg.h>
#include <ipc/dai.h>
#include <user/trace.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sof/audio/host_copier.h>
#include <sof/audio/dai_copier.h>
#include <sof/audio/ipcgtw_copier.h>

#if CONFIG_ZEPHYR_NATIVE_DRIVERS
#include <zephyr/drivers/dai.h>
#endif

static const struct comp_driver comp_copier;

LOG_MODULE_REGISTER(copier, CONFIG_SOF_LOG_LEVEL);

/* this id aligns windows driver requirement to support windows driver */
/* 9ba00c83-ca12-4a83-943c-1fa2e82f9dda */
DECLARE_SOF_RT_UUID("copier", copier_comp_uuid, 0x9ba00c83, 0xca12, 0x4a83,
		    0x94, 0x3c, 0x1f, 0xa2, 0xe8, 0x2f, 0x9d, 0xda);

DECLARE_TR_CTX(copier_comp_tr, SOF_UUID(copier_comp_uuid), LOG_LEVEL_INFO);

static struct comp_dev *copier_new(const struct comp_driver *drv,
				   const struct comp_ipc_config *config,
				   const void *spec)
{
	const struct ipc4_copier_module_cfg *copier = spec;
	union ipc4_connector_node_id node_id;
	struct ipc_comp_dev *ipc_pipe;
	struct ipc *ipc = ipc_get();
	struct copier_data *cd;
	struct comp_dev *dev;
	int i;

	comp_cl_dbg(&comp_copier, "copier_new()");

	dev = comp_alloc(drv, sizeof(*dev));
	if (!dev)
		return NULL;

	dev->ipc_config = *config;

	cd = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*cd));
	if (!cd)
		goto error;

	/*
	 * Don't copy the config_data[] variable size array, we don't need to
	 * store it, it's only used during IPC processing, besides we haven't
	 * allocated space for it, so don't "fix" this!
	 */
	if (memcpy_s(&cd->config, sizeof(cd->config), copier, sizeof(*copier)) < 0)
		goto error_cd;

	for (i = 0; i < IPC4_COPIER_MODULE_OUTPUT_PINS_COUNT; i++)
		cd->out_fmt[i] = cd->config.out_fmt;
	comp_set_drvdata(dev, cd);

	list_init(&dev->bsource_list);
	list_init(&dev->bsink_list);

	ipc_pipe = ipc_get_comp_by_ppl_id(ipc, COMP_TYPE_PIPELINE, config->pipeline_id);
	if (!ipc_pipe) {
		comp_cl_err(&comp_copier, "pipeline %d is not existed", config->pipeline_id);
		goto error_cd;
	}

	dev->pipeline = ipc_pipe->pipeline;

	node_id = copier->gtw_cfg.node_id;
	/* copier is linked to gateway */
	if (node_id.dw != IPC4_INVALID_NODE_ID) {
		cd->direction = get_gateway_direction(node_id.f.dma_type);

		switch (node_id.f.dma_type) {
		case ipc4_hda_host_output_class:
		case ipc4_hda_host_input_class:
			if (copier_host_create(dev, cd, &dev->ipc_config,
					       copier, cd->direction, ipc_pipe->pipeline)) {
				comp_cl_err(&comp_copier, "unable to create host");
				goto error_cd;
			}
			break;
		case ipc4_hda_link_output_class:
		case ipc4_hda_link_input_class:
		case ipc4_dmic_link_input_class:
		case ipc4_i2s_link_output_class:
		case ipc4_i2s_link_input_class:
		case ipc4_alh_link_output_class:
		case ipc4_alh_link_input_class:
			if (copier_dai_create(dev, cd, &dev->ipc_config,
					      copier, ipc_pipe->pipeline)) {
				comp_cl_err(&comp_copier, "unable to create dai");
				goto error_cd;
			}
			break;
#if CONFIG_IPC4_GATEWAY
		case ipc4_ipc_output_class:
		case ipc4_ipc_input_class:
			if (copier_ipcgtw_create(dev, cd, &dev->ipc_config,
						 copier, ipc_pipe->pipeline)) {
				comp_cl_err(&comp_copier, "unable to create IPC gateway");
				goto error_cd;
			}

			break;
#endif
		default:
			comp_cl_err(&comp_copier, "unsupported dma type %x",
				    (uint32_t)node_id.f.dma_type);
			goto error_cd;
		};

		dev->direction_set = true;
	}

	dev->direction = cd->direction;
	dev->state = COMP_STATE_READY;
	return dev;

error_cd:
	rfree(cd);
error:
	rfree(dev);
	return NULL;
}

static void copier_free(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);

	switch (dev->ipc_config.type) {
	case SOF_COMP_HOST:
		if (!cd->ipc_gtw)
			copier_host_free(cd);
		else
			/* handle gtw case */
			copier_ipcgtw_free(cd);
		break;
	case SOF_COMP_DAI:
		copier_dai_free(cd);
		break;
	default:
		break;
	}

	rfree(cd);
	rfree(dev);
}

static int copier_prepare(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	int ret;

	comp_dbg(dev, "copier_prepare()");

	/* cannot configure DAI while active */
	if (dev->state == COMP_STATE_ACTIVE) {
		comp_info(dev, "copier_config_prepare(): Component is in active state.");
		return 0;
	}

	ret = comp_set_state(dev, COMP_TRIGGER_PREPARE);
	if (ret < 0)
		return ret;

	if (ret == COMP_STATUS_STATE_ALREADY_SET)
		return PPL_STATUS_PATH_STOP;

	switch (dev->ipc_config.type) {
	case SOF_COMP_HOST:
		if (!cd->ipc_gtw) {
			ret = host_common_prepare(cd->hd);
			if (ret < 0)
				return ret;
		}
		break;
	case SOF_COMP_DAI:
		ret = copier_dai_prepare(dev, cd);
		if (ret < 0)
			return ret;
		break;
	default:
		break;
	}

	if (!cd->endpoint_num) {
		/* set up format conversion function for pin 0, for other pins (if any)
		 * format is set in IPC4_COPIER_MODULE_CFG_PARAM_SET_SINK_FORMAT handler
		 */
		cd->converter[0] = get_converter_func(&cd->config.base.audio_fmt,
							      &cd->config.out_fmt, ipc4_gtw_none,
							      ipc4_bidirection);
		if (!cd->converter[0]) {
			comp_err(dev, "can't support for in format %d, out format %d",
				 cd->config.base.audio_fmt.depth,  cd->config.out_fmt.depth);
			return -EINVAL;
		}
	}

	return 0;
}

static int copier_reset(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct ipc4_pipeline_registers pipe_reg;

	comp_dbg(dev, "copier_reset()");

	cd->input_total_data_processed = 0;
	cd->output_total_data_processed = 0;

	switch (dev->ipc_config.type) {
	case SOF_COMP_HOST:
		if (!cd->ipc_gtw)
			host_common_reset(cd->hd, dev->state);
		else
			copier_ipcgtw_reset(dev);
		break;
	case SOF_COMP_DAI:
		copier_dai_reset(cd, dev);
		break;
	default:
		break;
	}

	if (cd->pipeline_reg_offset) {
		pipe_reg.stream_start_offset = (uint64_t)-1;
		pipe_reg.stream_end_offset = (uint64_t)-1;
		mailbox_sw_regs_write(cd->pipeline_reg_offset, &pipe_reg, sizeof(pipe_reg));
	}

	comp_set_state(dev, COMP_TRIGGER_RESET);

	return 0;
}

static int copier_comp_trigger(struct comp_dev *dev, int cmd)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct sof_ipc_stream_posn posn;
	struct comp_dev *dai_copier;
	struct comp_buffer *buffer;
	struct comp_buffer __sparse_cache *buffer_c;
	uint32_t latency;
	int ret;

	comp_dbg(dev, "copier_comp_trigger()");

	ret = comp_set_state(dev, cmd);
	if (ret < 0)
		return ret;

	if (ret == COMP_STATUS_STATE_ALREADY_SET)
		return PPL_STATUS_PATH_STOP;

	switch (dev->ipc_config.type) {
	case SOF_COMP_HOST:
		if (!cd->ipc_gtw) {
			ret = host_common_trigger(cd->hd, dev, cmd);
			if (ret < 0)
				return ret;
		}
		break;
	case SOF_COMP_DAI:
		ret = copier_dai_trigger(cd, dev, cmd);
		break;
	default:
		break;
	}

	/* For capture cd->pipeline_reg_offset == 0 */
	if (!cd->endpoint_num || !cd->pipeline_reg_offset)
		return 0;

	dai_copier = pipeline_get_dai_comp_latency(dev->pipeline->pipeline_id, &latency);
	if (!dai_copier) {
		/*
		 * If the (playback) stream does not have a dai, the offset
		 * calculation is skipped
		 */
		comp_info(dev,
			  "No dai copier found, start/end offset is not calculated");
		return 0;
	}

	/* dai is in another pipeline and it is not prepared or active */
	if (dai_copier->state <= COMP_STATE_READY) {
		struct ipc4_pipeline_registers pipe_reg;

		comp_warn(dev, "dai is not ready");

		pipe_reg.stream_start_offset = 0;
		pipe_reg.stream_end_offset = 0;
		mailbox_sw_regs_write(cd->pipeline_reg_offset, &pipe_reg, sizeof(pipe_reg));

		return 0;
	}

	comp_position(dai_copier, &posn);

	/* update stream start and end offset for running message in host copier
	 * host driver uses DMA link counter to calculate stream position, but it is
	 * not fit for the following 3 cases:
	 * (1) dai is enabled before host since they are in different pipelines
	 * (2) multiple stream are mixed into one dai
	 * (3) one stream is paused but the dai is still working with other stream
	 *
	 * host uses stream_start_offset & stream_end_offset to deal with such cases:
	 * if (stream_start_offset) {
	 *	position = DMA counter - stream_start_offset
	 *	if (stream_stop_offset)
	 *		position = stream_stop_offset -stream_start_offset;
	 * } else {
	 *	position = 0;
	 * }
	 * When host component is started  stream_start_offset is set to DMA counter
	 * plus the latency from host to dai since the accurate DMA counter should be
	 * used when the data arrives dai from host.
	 *
	 * When pipeline is paused, stream_end_offset will save current DMA counter
	 * for resume case
	 *
	 * When pipeline is resumed, calculate stream_start_offset based on current
	 * DMA counter and stream_end_offset
	 */
	if (cmd == COMP_TRIGGER_START) {
		struct ipc4_pipeline_registers pipe_reg;

		if (list_is_empty(&dai_copier->bsource_list)) {
			comp_err(dev, "No source buffer bound to dai_copier");
			return -EINVAL;
		}

		buffer = list_first_item(&dai_copier->bsource_list, struct comp_buffer, sink_list);
		buffer_c = buffer_acquire(buffer);
		pipe_reg.stream_start_offset = posn.dai_posn +
			latency * audio_stream_get_size(&buffer_c->stream);
		buffer_release(buffer_c);
		pipe_reg.stream_end_offset = 0;
		mailbox_sw_regs_write(cd->pipeline_reg_offset, &pipe_reg, sizeof(pipe_reg));
	} else if (cmd == COMP_TRIGGER_PAUSE) {
		uint64_t stream_end_offset;

		stream_end_offset = posn.dai_posn;
		mailbox_sw_regs_write(cd->pipeline_reg_offset + sizeof(uint64_t),
				      &stream_end_offset, sizeof(stream_end_offset));
	} else if (cmd == COMP_TRIGGER_RELEASE) {
		struct ipc4_pipeline_registers pipe_reg;

		pipe_reg.stream_start_offset = mailbox_sw_reg_read64(cd->pipeline_reg_offset);
		pipe_reg.stream_end_offset = mailbox_sw_reg_read64(cd->pipeline_reg_offset +
			sizeof(pipe_reg.stream_start_offset));
		pipe_reg.stream_start_offset += posn.dai_posn - pipe_reg.stream_end_offset;

		if (list_is_empty(&dai_copier->bsource_list)) {
			comp_err(dev, "No source buffer bound to dai_copier");
			return -EINVAL;
		}

		buffer = list_first_item(&dai_copier->bsource_list, struct comp_buffer, sink_list);
		buffer_c = buffer_acquire(buffer);
		pipe_reg.stream_start_offset += latency * audio_stream_get_size(&buffer_c->stream);
		buffer_release(buffer_c);
		mailbox_sw_regs_write(cd->pipeline_reg_offset, &pipe_reg.stream_start_offset,
				      sizeof(pipe_reg.stream_start_offset));
	}

	return ret;
}

static inline struct comp_buffer *get_endpoint_buffer(struct copier_data *cd)
{
	return cd->multi_endpoint_buffer ? cd->multi_endpoint_buffer :
		cd->endpoint_buffer[IPC4_COPIER_GATEWAY_PIN];
}

static int do_endpoint_copy(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);

	switch (dev->ipc_config.type) {
	case SOF_COMP_HOST:
		if (!cd->ipc_gtw)
			return host_common_copy(cd->hd, dev, copier_host_dma_cb);
		break;
	case SOF_COMP_DAI:
		if (cd->endpoint_num == 1)
			return dai_common_copy(cd->dd[0], dev, cd->converter);

		return dai_zephyr_multi_endpoint_copy(cd->dd, dev, cd->multi_endpoint_buffer,
						      cd->endpoint_num);
	default:
		break;
	}
	return 0;
}

static int do_conversion_copy(struct comp_dev *dev,
			      struct copier_data *cd,
			      struct comp_buffer __sparse_cache *src,
			      struct comp_buffer __sparse_cache *sink,
			      struct comp_copy_limits *processed_data)
{
	int i;
	int ret;

	/* buffer params might be not yet configured by component on another pipeline */
	if (!src->hw_params_configured || !sink->hw_params_configured)
		return 0;

	comp_get_copy_limits(src, sink, processed_data);

	i = IPC4_SINK_QUEUE_ID(sink->id);
	if (i >= IPC4_COPIER_MODULE_OUTPUT_PINS_COUNT)
		return -EINVAL;
	buffer_stream_invalidate(src, processed_data->source_bytes);

	cd->converter[i](&src->stream, 0, &sink->stream, 0,
			 processed_data->frames * audio_stream_get_channels(&sink->stream));

	if (cd->attenuation) {
		ret = apply_attenuation(dev, cd, sink, processed_data->frames);
		if (ret < 0)
			return ret;
	}

	buffer_stream_writeback(sink, processed_data->sink_bytes);
	comp_update_buffer_produce(sink, processed_data->sink_bytes);

	return 0;
}

/* Copier has one input and one or more outputs. Maximum of one gateway can be connected
 * to copier or no gateway connected at all. Gateway can only be connected to either input
 * pin 0 (the only input) or output pin 0. With or without connected gateway it is also
 * possible to have component(s) connected on input and/or output pins.
 *
 * A special exception is a multichannel ALH gateway case. These are multiple gateways
 * but should be treated like a single gateway to satisfy rules above. Data from such
 * gateways has to be multiplexed into single stream (for input gateways) or demultiplexed
 * from single stream (for output gateways) so such gateways work kind of like a single
 * gateway, i.e., produce/consume single stream.
 */
static int copier_copy(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *src, *sink;
	struct comp_buffer __sparse_cache *src_c, *sink_c;
	struct comp_copy_limits processed_data;
	struct list_item *sink_list;
	int ret = 0;

	comp_dbg(dev, "copier_copy()");

	switch (dev->ipc_config.type) {
	case SOF_COMP_HOST:
		if (!cd->ipc_gtw)
			return do_endpoint_copy(dev);
		break;
	case SOF_COMP_DAI:
		if (cd->endpoint_num == 1)
			return do_endpoint_copy(dev);
		break;
	default:
		break;
	}

	processed_data.source_bytes = 0;

	if (cd->endpoint_num && !cd->bsource_buffer) {
		/* gateway(s) as input */
		ret = do_endpoint_copy(dev);
		if (ret < 0)
			return ret;

		src_c = buffer_acquire(get_endpoint_buffer(cd));
	} else {
		/* component as input */
		if (list_is_empty(&dev->bsource_list)) {
			comp_err(dev, "No source buffer bound");
			return -EINVAL;
		}

		src = list_first_item(&dev->bsource_list, struct comp_buffer, sink_list);
		src_c = buffer_acquire(src);

		if (cd->endpoint_num) {
			/* gateway(s) on output */
			sink_c = buffer_acquire(get_endpoint_buffer(cd));
			ret = do_conversion_copy(dev, cd, src_c, sink_c, &processed_data);
			buffer_release(sink_c);

			if (ret < 0) {
				buffer_release(src_c);
				return ret;
			}

			ret = do_endpoint_copy(dev);
			if (ret < 0) {
				buffer_release(src_c);
				return ret;
			}
		}
	}

	/* zero or more components on outputs */
	list_for_item(sink_list, &dev->bsink_list) {
		struct comp_dev *sink_dev;

		sink = container_of(sink_list, struct comp_buffer, source_list);
		sink_c = buffer_acquire(sink);
		sink_dev = sink_c->sink;
		processed_data.sink_bytes = 0;
		if (sink_dev->state == COMP_STATE_ACTIVE) {
			ret = do_conversion_copy(dev, cd, src_c, sink_c, &processed_data);
			cd->output_total_data_processed += processed_data.sink_bytes;
		}
		buffer_release(sink_c);
		if (ret < 0) {
			comp_err(dev, "failed to copy buffer for comp %x",
				 dev->ipc_config.id);
			break;
		}
	}

	if (!ret) {
		comp_update_buffer_consume(src_c, processed_data.source_bytes);
		if (!cd->endpoint_num || cd->bsource_buffer)
			cd->input_total_data_processed += processed_data.source_bytes;
	}

	buffer_release(src_c);

	return ret;
}

/* configure the DMA params */
static int copier_params(struct comp_dev *dev, struct sof_ipc_stream_params *params)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	int i, ret = 0;

	comp_dbg(dev, "copier_params()");

	copier_update_params(cd, dev, params);

	for (i = 0; i < cd->endpoint_num; i++) {
		switch (dev->ipc_config.type) {
		case SOF_COMP_HOST:
			if (!cd->ipc_gtw)
				ret = copier_host_params(cd, dev, params);
			else
				/* handle gtw case */
				ret = copier_ipcgtw_params(cd->ipcgtw_data, dev, params);
			break;
		case SOF_COMP_DAI:
			ret = copier_dai_params(cd, dev, params, i);
			break;
		default:
			break;
		}

		if (ret < 0)
			break;
	}

	return ret;
}

static int copier_set_sink_fmt(struct comp_dev *dev, const void *data,
			       int max_data_size)
{
	const struct ipc4_copier_config_set_sink_format *sink_fmt = data;
	struct copier_data *cd = comp_get_drvdata(dev);
	struct comp_buffer __sparse_cache *sink_c;
	struct list_item *sink_list;
	struct comp_buffer *sink;

	if (max_data_size < sizeof(*sink_fmt)) {
		comp_err(dev, "error: max_data_size %d should be bigger than %d", max_data_size,
			 sizeof(*sink_fmt));
		return -EINVAL;
	}

	if (sink_fmt->sink_id >= IPC4_COPIER_MODULE_OUTPUT_PINS_COUNT) {
		comp_err(dev, "error: sink id %d is out of range", sink_fmt->sink_id);
		return -EINVAL;
	}

	if (memcmp(&cd->config.base.audio_fmt, &sink_fmt->source_fmt,
		   sizeof(sink_fmt->source_fmt))) {
		comp_err(dev, "error: source fmt should be equal to input fmt");
		return -EINVAL;
	}

	if (cd->endpoint_num && cd->bsource_buffer &&
	    sink_fmt->sink_id == IPC4_COPIER_GATEWAY_PIN) {
		comp_err(dev, "can't change gateway format");
		return -EINVAL;
	}

	cd->out_fmt[sink_fmt->sink_id] = sink_fmt->sink_fmt;
	cd->converter[sink_fmt->sink_id] = get_converter_func(&sink_fmt->source_fmt,
							      &sink_fmt->sink_fmt, ipc4_gtw_none,
							      ipc4_bidirection);

	/* update corresponding sink format */
	list_for_item(sink_list, &dev->bsink_list) {
		int sink_id;

		sink = container_of(sink_list, struct comp_buffer, source_list);
		sink_c = buffer_acquire(sink);

		sink_id = IPC4_SINK_QUEUE_ID(sink_c->id);
		if (sink_id == sink_fmt->sink_id) {
			update_buffer_format(sink_c, &sink_fmt->sink_fmt);
			buffer_release(sink_c);
			break;
		}

		buffer_release(sink_c);
	}

	return 0;
}

static int set_attenuation(struct comp_dev *dev, uint32_t data_offset, const char *data)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	uint32_t attenuation;
	enum sof_ipc_frame valid_fmt, frame_fmt;

	/* only support attenuation in format of 32bit */
	if (data_offset > sizeof(uint32_t)) {
		comp_err(dev, "attenuation data size %d is incorrect", data_offset);
		return -EINVAL;
	}

	attenuation = *(const uint32_t *)data;
	if (attenuation > 31) {
		comp_err(dev, "attenuation %d is out of range", attenuation);
		return -EINVAL;
	}

	audio_stream_fmt_conversion(cd->config.base.audio_fmt.depth,
				    cd->config.base.audio_fmt.valid_bit_depth,
				    &frame_fmt, &valid_fmt,
				    cd->config.base.audio_fmt.s_type);

	if (frame_fmt < SOF_IPC_FRAME_S24_4LE) {
		comp_err(dev, "frame_fmt %d isn't supported by attenuation",
			 frame_fmt);
		return -EINVAL;
	}

	cd->attenuation = attenuation;

	return 0;
}

static int copier_set_large_config(struct comp_dev *dev, uint32_t param_id,
				   bool first_block,
				   bool last_block,
				   uint32_t data_offset,
				   const char *data)
{
	comp_dbg(dev, "copier_set_large_config()");

	switch (param_id) {
	case IPC4_COPIER_MODULE_CFG_PARAM_SET_SINK_FORMAT:
		return copier_set_sink_fmt(dev, data, data_offset);
	case IPC4_COPIER_MODULE_CFG_ATTENUATION:
		return set_attenuation(dev, data_offset, data);
	default:
		return -EINVAL;
	}
}

static inline void convert_u64_to_u32s(uint64_t val, uint32_t *val_l, uint32_t *val_h)
{
	*val_l = (uint32_t)(val & 0xffffffff);
	*val_h = (uint32_t)((val >> 32) & 0xffffffff);
}

static int copier_get_large_config(struct comp_dev *dev, uint32_t param_id,
				   bool first_block,
				   bool last_block,
				   uint32_t *data_offset,
				   char *data)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct sof_ipc_stream_posn posn;
	struct ipc4_llp_reading_extended llp_ext;
	struct ipc4_llp_reading llp;

	if (cd->ipc_gtw)
		return 0;

	switch (param_id) {
	case IPC4_COPIER_MODULE_CFG_PARAM_LLP_READING:
		if (!cd->endpoint_num ||
		    comp_get_endpoint_type(dev) !=
		    COMP_ENDPOINT_DAI) {
			comp_err(dev, "Invalid component type");
			return -EINVAL;
		}

		if (*data_offset < sizeof(struct ipc4_llp_reading)) {
			comp_err(dev, "Config size %d is inadequate", *data_offset);
			return -EINVAL;
		}

		*data_offset = sizeof(struct ipc4_llp_reading);
		memset(&llp, 0, sizeof(llp));

		if (dev->state != COMP_STATE_ACTIVE) {
			memcpy_s(data, sizeof(llp), &llp, sizeof(llp));
			return 0;
		}

		/* get llp from dai */
		comp_position(dev, &posn);

		convert_u64_to_u32s(posn.comp_posn, &llp.llp_l, &llp.llp_u);
		convert_u64_to_u32s(posn.wallclock, &llp.wclk_l, &llp.wclk_u);
		memcpy_s(data, sizeof(llp), &llp, sizeof(llp));

		return 0;

	case IPC4_COPIER_MODULE_CFG_PARAM_LLP_READING_EXTENDED:
		if (!cd->endpoint_num ||
		    comp_get_endpoint_type(dev) !=
		    COMP_ENDPOINT_DAI) {
			comp_err(dev, "Invalid component type");
			return -EINVAL;
		}

		if (*data_offset < sizeof(struct ipc4_llp_reading_extended)) {
			comp_err(dev, "Config size %d is inadequate", *data_offset);
			return -EINVAL;
		}

		*data_offset = sizeof(struct ipc4_llp_reading_extended);
		memset(&llp_ext, 0, sizeof(llp_ext));

		if (dev->state != COMP_STATE_ACTIVE) {
			memcpy_s(data, sizeof(llp_ext), &llp_ext, sizeof(llp_ext));
			return 0;
		}

		/* get llp from dai */
		comp_position(dev, &posn);

		convert_u64_to_u32s(posn.comp_posn, &llp_ext.llp_reading.llp_l,
				    &llp_ext.llp_reading.llp_u);
		convert_u64_to_u32s(posn.wallclock, &llp_ext.llp_reading.wclk_l,
				    &llp_ext.llp_reading.wclk_u);

		convert_u64_to_u32s(posn.dai_posn, &llp_ext.tpd_low, &llp_ext.tpd_high);
		memcpy_s(data, sizeof(llp_ext), &llp_ext, sizeof(llp_ext));

		return 0;

	default:
		comp_err(dev, "unsupported param %d", param_id);
		break;
	}

	return -EINVAL;
}

static uint64_t copier_get_processed_data(struct comp_dev *dev, uint32_t stream_no, bool input)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	uint64_t ret = 0;
	bool source;

	/*
	 * total data processed is calculated as: accumulate all input data in bytes
	 * from host, then store in host_data structure, this function intend to retrieve
	 * correct processed data number.
	 * Due to host already integrated as part of copier, so for host specific case,
	 * as long as direction is correct, return correct processed data.
	 * Dai still use ops driver to get data, later, dai will also be integrated
	 * into copier, this part will be changed again for dai specific.
	 */
	if (cd->endpoint_num) {
		if (stream_no < cd->endpoint_num) {
			switch (dev->ipc_config.type) {
			case  SOF_COMP_HOST:
				source = dev->direction == SOF_IPC_STREAM_PLAYBACK;
				/* only support host, not support ipcgtw case */
				if (!cd->ipc_gtw && source == input)
					ret = cd->hd->total_data_processed;
				break;
			case  SOF_COMP_DAI:
				source = dev->direction == SOF_IPC_STREAM_CAPTURE;
				if (source == input)
					ret = cd->dd[0]->total_data_processed;
				break;
			default:
				ret = comp_get_total_data_processed(cd->endpoint[stream_no],
								    0, input);
				break;
			}
		}
	} else {
		if (stream_no == 0)
			ret = input ? cd->input_total_data_processed :
				      cd->output_total_data_processed;
	}

	return ret;
}

static int copier_get_attribute(struct comp_dev *dev, uint32_t type, void *value)
{
	struct copier_data *cd = comp_get_drvdata(dev);

	switch (type) {
	case COMP_ATTR_BASE_CONFIG:
		*(struct ipc4_base_module_cfg *)value = cd->config.base;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int copier_position(struct comp_dev *dev, struct sof_ipc_stream_posn *posn)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	int ret = 0;

	/* Exit if no endpoints */
	if (!cd->endpoint_num)
		return -EINVAL;

	switch (dev->ipc_config.type) {
	case SOF_COMP_HOST:
		/* only support host not support gtw case */
		if (!cd->ipc_gtw) {
			posn->host_posn = cd->hd->local_pos;
			ret = posn->host_posn;
		}
		break;
	case SOF_COMP_DAI:
		ret = dai_common_position(cd->dd[0], dev, posn);
		break;
	default:
		break;
	}
	/* Return position from the default gateway pin */
	return ret;
}

static int copier_dai_ts_config_op(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct dai_data *dd = cd->dd[0];

	return dai_common_ts_config_op(dd, dev);
}

static int copier_dai_ts_start_op(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct dai_data *dd = cd->dd[0];

	comp_dbg(dev, "dai_ts_start()");

	return dai_common_ts_start(dd, dev);
}

static int copier_dai_ts_get_op(struct comp_dev *dev, struct timestamp_data *tsd)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct dai_data *dd = cd->dd[0];

	comp_dbg(dev, "dai_ts_get()");

	return dai_common_ts_get(dd, dev, tsd);
}

static int copier_dai_ts_stop_op(struct comp_dev *dev)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct dai_data *dd = cd->dd[0];

	comp_dbg(dev, "dai_ts_stop()");

	return dai_common_ts_stop(dd, dev);
}

static int copier_get_hw_params(struct comp_dev *dev, struct sof_ipc_stream_params *params,
				int dir)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct dai_data *dd = cd->dd[0];

	if (dev->ipc_config.type != SOF_COMP_DAI)
		return -EINVAL;

	return dai_common_get_hw_params(dd, dev, params, dir);
}

static int copier_unbind(struct comp_dev *dev, void *data)
{
	struct copier_data *cd = comp_get_drvdata(dev);
	struct dai_data *dd = cd->dd[0];

	if (dev->ipc_config.type == SOF_COMP_DAI)
		return dai_zephyr_unbind(dd, dev, data);

	return 0;
}

static const struct comp_driver comp_copier = {
	.uid	= SOF_RT_UUID(copier_comp_uuid),
	.tctx	= &copier_comp_tr,
	.ops	= {
		.create				= copier_new,
		.free				= copier_free,
		.trigger			= copier_comp_trigger,
		.copy				= copier_copy,
		.set_large_config		= copier_set_large_config,
		.get_large_config		= copier_get_large_config,
		.params				= copier_params,
		.prepare			= copier_prepare,
		.reset				= copier_reset,
		.get_total_data_processed	= copier_get_processed_data,
		.get_attribute			= copier_get_attribute,
		.position			= copier_position,
		.dai_ts_config			= copier_dai_ts_config_op,
		.dai_ts_start			= copier_dai_ts_start_op,
		.dai_ts_stop			= copier_dai_ts_stop_op,
		.dai_ts_get			= copier_dai_ts_get_op,
		.dai_get_hw_params		= copier_get_hw_params,
		.unbind			= copier_unbind,
	},
};

static SHARED_DATA struct comp_driver_info comp_copier_info = {
	.drv = &comp_copier,
};

UT_STATIC void sys_comp_copier_init(void)
{
	comp_register(platform_shared_get(&comp_copier_info,
					  sizeof(comp_copier_info)));
}

DECLARE_MODULE(sys_comp_copier_init);
SOF_MODULE_INIT(copier, sys_comp_copier_init);
