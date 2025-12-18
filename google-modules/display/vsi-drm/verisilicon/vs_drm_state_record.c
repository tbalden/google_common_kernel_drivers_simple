// SPDX-License-Identifier: MIT

#include "vs_drm_state_record.h"
#include "vs_drm_atomic.h"

#include <drm/drm_crtc.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane.h>
#include <drm/drm_writeback.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>

#include <trace/dpu_trace.h>

/**
 * struct drm_state_history_record - a record of most recent states
 *
 * Keeps references to struct drm_atomic_state objects in a small ring buffer,
 * such that their contents may be dumped by vs_drm_recorded_states_prepare()
 *
 * @state_ringbuf: ring buffer of previous states
 * @state_ringbuf_idx: the next spot to store a state
 * @state_ringbuf_mutex: mutex controlling access to the other members
 */
struct drm_state_history_record {
	struct drm_atomic_state *state_ringbuf[VS_RECORD_STATE_MAX];
	int state_ringbuf_idx; /* the next spot to store a state */
	struct mutex state_ringbuf_mutex;
};

int vs_drm_prepare_state_history_record(struct drm_state_history_record **sh_record)
{
	*sh_record = vzalloc(sizeof(struct drm_state_history_record));
	if (!(*sh_record))
		return -ENOMEM;

	mutex_init(&((*sh_record)->state_ringbuf_mutex));

	return 0;
}

void vs_drm_destroy_state_history_record(struct drm_state_history_record *sh_record)
{
	if (sh_record) {
		mutex_destroy(&sh_record->state_ringbuf_mutex);

		vfree(sh_record);
	}
}

/* State Tracking and Dumping */

#if IS_ENABLED(CONFIG_VERISILICON_RECORD_DRM_STATE)
void vs_drm_record_state(struct drm_atomic_state *state, struct drm_state_history_record *sh_record)
{
	struct drm_atomic_state *state_to_drop;

	mutex_lock(&sh_record->state_ringbuf_mutex);

	/* clear old contents */
	state_to_drop = sh_record->state_ringbuf[sh_record->state_ringbuf_idx];
	if (state_to_drop != NULL)
		drm_atomic_state_put(state_to_drop);

	/* put new state in */
	sh_record->state_ringbuf[sh_record->state_ringbuf_idx] = drm_atomic_state_get(state);

	/* increment ringbuf idx */
	sh_record->state_ringbuf_idx = (sh_record->state_ringbuf_idx + 1) % VS_RECORD_STATE_MAX;
	mutex_unlock(&sh_record->state_ringbuf_mutex);
}
#else
void vs_drm_record_state(struct drm_atomic_state *state, struct drm_state_history_record *sh_record)
{
}
#endif

static size_t __dump_old_state(char *buffer, size_t count, struct drm_atomic_state *old_state)
{
	struct drm_print_iterator iter;
	struct drm_printer p;

	iter.data = buffer;
	iter.start = 0;
	iter.remain = count;

	p = drm_coredump_printer(&iter);

	vs_drm_atomic_print_old_state(old_state, &p);

	return count - iter.remain;
}

static int dump_old_state(char **out_buffer, size_t *out_buffer_size,
			  struct drm_atomic_state *old_state)
{
	size_t count;

	if (!out_buffer || !out_buffer_size)
		return -EINVAL;

	DPU_ATRACE_BEGIN("vs_drm_dump_old_state");

	/* first call is to find buffer size */
	count = __dump_old_state(NULL, INT_MAX, old_state);
	*out_buffer = vzalloc(count + 1);
	if (!out_buffer)
		return -ENOMEM;
	/* second call writes to buffer */
	__dump_old_state(*out_buffer, count, old_state);
	*out_buffer_size = count;

	DPU_ATRACE_END("vs_drm_dump_old_state");

	return 0;
}

/*
 * Note that addition of VS_RECORD_STATE_MAX gets offset by modulo,
 * but still helpful for negative math when idx = [0..VS_RECORD_STATE_MAX]
 */
static inline int ringbuf_offset(unsigned int index, int offset)
{
	return ((index + VS_RECORD_STATE_MAX + offset) % VS_RECORD_STATE_MAX);
}
static inline int ringbuf_prev(unsigned int index)
{
	return ringbuf_offset(index, -1);
}

#define for_state_in_ringbuf_reverse(state_ringbuf, state_to_dump, state_ringbuf_idx, i) \
	for (i = 0, state_to_dump = (state_ringbuf)[ringbuf_prev(state_ringbuf_idx)];    \
	     i < VS_RECORD_STATE_MAX;                                                    \
	     i++, state_to_dump = (state_ringbuf)[ringbuf_offset((state_ringbuf_idx), -1 - i)])

static int vs_drm_dump_recorded_states(char **out_buffers, size_t *out_buffer_sizes,
				       int max_out_buffers,
				       struct drm_state_history_record *sh_record)
{
	int out_buffer_count = 0;
	struct drm_atomic_state *state_to_dump;
	int i;

	mutex_lock(&sh_record->state_ringbuf_mutex);

	for_state_in_ringbuf_reverse(sh_record->state_ringbuf, state_to_dump,
				     sh_record->state_ringbuf_idx, i)
	{
		int ret;

		if (out_buffer_count >= max_out_buffers)
			break;
		if (!state_to_dump)
			continue;
		ret = dump_old_state(out_buffers + out_buffer_count,
				     out_buffer_sizes + out_buffer_count, state_to_dump);
		if (!ret) {
			out_buffer_count++;
		} else if (ret == -ENOMEM) {
			int j;

			for (j = 0; j < out_buffer_count; j++)
				vfree(out_buffers + j);
			out_buffer_count = ret;
			break;
		}
	}
	mutex_unlock(&sh_record->state_ringbuf_mutex);

	return out_buffer_count;
}

int vs_drm_recorded_states_prepare(struct drm_state_history_data *sh_data,
				   struct drm_state_history_record *sh_record)
{
	int num_logged_states;

	if (!sh_data || !sh_record)
		return -EINVAL;

	num_logged_states = vs_drm_dump_recorded_states(sh_data->buffers, sh_data->buffer_sizes,
							VS_RECORD_STATE_MAX, sh_record);

	sh_data->num_logged_states = num_logged_states;

	return num_logged_states;
}

void vs_drm_recorded_states_destroy(struct drm_state_history_data *sh_data)
{
	int i;

	if (!sh_data)
		return;

	for (i = 0; i < sh_data->num_logged_states; ++i)
		vfree(sh_data->buffers[i]);
}
