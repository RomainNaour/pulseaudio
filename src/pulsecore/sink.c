/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <pulse/introspect.h>
#include <pulse/utf8.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/play-memblockq.h>

#include "sink.h"

#define MAX_MIX_CHANNELS 32
#define MIX_BUFFER_LENGTH (PA_PAGE_SIZE)
#define DEFAULT_MIN_LATENCY (4*PA_USEC_PER_MSEC)

static PA_DEFINE_CHECK_TYPE(pa_sink, pa_msgobject);

static void sink_free(pa_object *s);

pa_sink_new_data* pa_sink_new_data_init(pa_sink_new_data *data) {
    pa_assert(data);

    memset(data, 0, sizeof(*data));
    data->proplist = pa_proplist_new();

    return data;
}

void pa_sink_new_data_set_name(pa_sink_new_data *data, const char *name) {
    pa_assert(data);

    pa_xfree(data->name);
    data->name = pa_xstrdup(name);
}

void pa_sink_new_data_set_sample_spec(pa_sink_new_data *data, const pa_sample_spec *spec) {
    pa_assert(data);

    if ((data->sample_spec_is_set = !!spec))
        data->sample_spec = *spec;
}

void pa_sink_new_data_set_channel_map(pa_sink_new_data *data, const pa_channel_map *map) {
    pa_assert(data);

    if ((data->channel_map_is_set = !!map))
        data->channel_map = *map;
}

void pa_sink_new_data_set_volume(pa_sink_new_data *data, const pa_cvolume *volume) {
    pa_assert(data);

    if ((data->volume_is_set = !!volume))
        data->volume = *volume;
}

void pa_sink_new_data_set_muted(pa_sink_new_data *data, pa_bool_t mute) {
    pa_assert(data);

    data->muted_is_set = TRUE;
    data->muted = !!mute;
}

void pa_sink_new_data_done(pa_sink_new_data *data) {
    pa_assert(data);

    pa_xfree(data->name);
    pa_proplist_free(data->proplist);
}

static void reset_callbacks(pa_sink *s) {
    pa_assert(s);

    s->set_state = NULL;
    s->get_volume = NULL;
    s->set_volume = NULL;
    s->get_mute = NULL;
    s->set_mute = NULL;
    s->get_latency = NULL;
    s->request_rewind = NULL;
    s->update_requested_latency = NULL;
}

pa_sink* pa_sink_new(
        pa_core *core,
        pa_sink_new_data *data,
        pa_sink_flags_t flags) {

    pa_sink *s;
    const char *name;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX], cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_source_new_data source_data;
    const char *dn;

    pa_assert(core);
    pa_assert(data);
    pa_assert(data->name);

    s = pa_msgobject_new(pa_sink);

    if (!(name = pa_namereg_register(core, data->name, PA_NAMEREG_SINK, s, data->namereg_fail))) {
        pa_xfree(s);
        return NULL;
    }

    pa_sink_new_data_set_name(data, name);

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_NEW], data) < 0) {
        pa_xfree(s);
        pa_namereg_unregister(core, name);
        return NULL;
    }

    pa_return_null_if_fail(!data->driver || pa_utf8_valid(data->driver));
    pa_return_null_if_fail(data->name && pa_utf8_valid(data->name) && data->name[0]);

    pa_return_null_if_fail(data->sample_spec_is_set && pa_sample_spec_valid(&data->sample_spec));

    if (!data->channel_map_is_set)
        pa_return_null_if_fail(pa_channel_map_init_auto(&data->channel_map, data->sample_spec.channels, PA_CHANNEL_MAP_DEFAULT));

    pa_return_null_if_fail(pa_channel_map_valid(&data->channel_map));
    pa_return_null_if_fail(data->channel_map.channels == data->sample_spec.channels);

    if (!data->volume_is_set)
        pa_cvolume_reset(&data->volume, data->sample_spec.channels);

    pa_return_null_if_fail(pa_cvolume_valid(&data->volume));
    pa_return_null_if_fail(data->volume.channels == data->sample_spec.channels);

    if (!data->muted_is_set)
        data->muted = FALSE;

    if (pa_hook_fire(&core->hooks[PA_CORE_HOOK_SINK_FIXATE], data) < 0) {
        pa_xfree(s);
        pa_namereg_unregister(core, name);
        return NULL;
    }

    s->parent.parent.free = sink_free;
    s->parent.process_msg = pa_sink_process_msg;

    s->core = core;
    s->state = PA_SINK_INIT;
    s->flags = flags;
    s->name = pa_xstrdup(name);
    s->proplist = pa_proplist_copy(data->proplist);
    s->driver = pa_xstrdup(data->driver);
    s->module = data->module;

    s->sample_spec = data->sample_spec;
    s->channel_map = data->channel_map;

    s->inputs = pa_idxset_new(NULL, NULL);
    s->n_corked = 0;

    s->volume = data->volume;
    s->muted = data->muted;
    s->refresh_volume = s->refresh_mute = FALSE;

    reset_callbacks(s);
    s->userdata = NULL;

    s->asyncmsgq = NULL;
    s->rtpoll = NULL;

    pa_silence_memchunk_get(
            &core->silence_cache,
            core->mempool,
            &s->silence,
            &s->sample_spec,
            0);

    s->min_latency = DEFAULT_MIN_LATENCY;
    s->max_latency = s->min_latency;

    s->thread_info.inputs = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    s->thread_info.soft_volume = s->volume;
    s->thread_info.soft_muted = s->muted;
    s->thread_info.state = s->state;
    s->thread_info.rewind_nbytes = 0;
    s->thread_info.max_rewind = 0;
    s->thread_info.requested_latency_valid = FALSE;
    s->thread_info.requested_latency = 0;

    pa_assert_se(pa_idxset_put(core->sinks, s, &s->index) >= 0);

    pa_log_info("Created sink %u \"%s\" with sample spec %s and channel map %s",
                s->index,
                s->name,
                pa_sample_spec_snprint(st, sizeof(st), &s->sample_spec),
                pa_channel_map_snprint(cm, sizeof(cm), &s->channel_map));

    pa_source_new_data_init(&source_data);
    pa_source_new_data_set_sample_spec(&source_data, &s->sample_spec);
    pa_source_new_data_set_channel_map(&source_data, &s->channel_map);
    source_data.name = pa_sprintf_malloc("%s.monitor", name);
    source_data.driver = data->driver;
    source_data.module = data->module;

    dn = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_DESCRIPTION);
    pa_proplist_setf(source_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Monitor of %s", dn ? dn : s->name);
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_CLASS, "monitor");

    s->monitor_source = pa_source_new(core, &source_data, 0);

    pa_source_new_data_done(&source_data);

    if (!s->monitor_source) {
        pa_sink_unlink(s);
        pa_sink_unref(s);
        return NULL;
    }

    s->monitor_source->monitor_of = s;
    pa_source_set_max_rewind(s->monitor_source, s->thread_info.max_rewind);

    return s;
}

static int sink_set_state(pa_sink *s, pa_sink_state_t state) {
    int ret;
    pa_bool_t suspend_change;

    pa_assert(s);

    if (s->state == state)
        return 0;

    suspend_change =
        (s->state == PA_SINK_SUSPENDED && PA_SINK_OPENED(state)) ||
        (PA_SINK_OPENED(s->state) && state == PA_SINK_SUSPENDED);

    if (s->set_state)
        if ((ret = s->set_state(s, state)) < 0)
            return -1;

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_STATE, PA_UINT_TO_PTR(state), 0, NULL) < 0)
        return -1;

    s->state = state;

    if (suspend_change) {
        pa_sink_input *i;
        uint32_t idx;

        /* We're suspending or resuming, tell everyone about it */

        for (i = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); i; i = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx)))
            if (i->suspend)
                i->suspend(i, state == PA_SINK_SUSPENDED);
    }

    if (state != PA_SINK_UNLINKED) /* if we enter UNLINKED state pa_sink_unlink() will fire the apropriate events */
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], s);

    return 0;
}

void pa_sink_put(pa_sink* s) {
    pa_sink_assert_ref(s);

    pa_assert(s->state == PA_SINK_INIT);
    pa_assert(s->asyncmsgq);
    pa_assert(s->rtpoll);

    pa_assert(!s->min_latency || !s->max_latency || s->min_latency <= s->max_latency);

    if (!(s->flags & PA_SINK_HW_VOLUME_CTRL))
        s->flags |= PA_SINK_DECIBEL_VOLUME;

    pa_assert_se(sink_set_state(s, PA_SINK_IDLE) == 0);

    pa_source_put(s->monitor_source);

    pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_NEW, s->index);
    pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_PUT], s);
}

void pa_sink_unlink(pa_sink* s) {
    pa_bool_t linked;
    pa_sink_input *i, *j = NULL;

    pa_assert(s);

    /* Please note that pa_sink_unlink() does more than simply
     * reversing pa_sink_put(). It also undoes the registrations
     * already done in pa_sink_new()! */

    /* All operations here shall be idempotent, i.e. pa_sink_unlink()
     * may be called multiple times on the same sink without bad
     * effects. */

    linked = PA_SINK_LINKED(s->state);

    if (linked)
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_UNLINK], s);

    if (s->state != PA_SINK_UNLINKED)
        pa_namereg_unregister(s->core, s->name);
    pa_idxset_remove_by_data(s->core->sinks, s, NULL);

    while ((i = pa_idxset_first(s->inputs, NULL))) {
        pa_assert(i != j);
        pa_sink_input_kill(i);
        j = i;
    }

    if (linked)
        sink_set_state(s, PA_SINK_UNLINKED);
    else
        s->state = PA_SINK_UNLINKED;

    reset_callbacks(s);

    if (s->monitor_source)
        pa_source_unlink(s->monitor_source);

    if (linked) {
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK | PA_SUBSCRIPTION_EVENT_REMOVE, s->index);
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_UNLINK_POST], s);
    }
}

static void sink_free(pa_object *o) {
    pa_sink *s = PA_SINK(o);
    pa_sink_input *i;

    pa_assert(s);
    pa_assert(pa_sink_refcnt(s) == 0);

    if (PA_SINK_LINKED(s->state))
        pa_sink_unlink(s);

    pa_log_info("Freeing sink %u \"%s\"", s->index, s->name);

    if (s->monitor_source) {
        pa_source_unref(s->monitor_source);
        s->monitor_source = NULL;
    }

    pa_idxset_free(s->inputs, NULL, NULL);

    while ((i = pa_hashmap_steal_first(s->thread_info.inputs)))
        pa_sink_input_unref(i);

    pa_hashmap_free(s->thread_info.inputs, NULL, NULL);

    if (s->silence.memblock)
        pa_memblock_unref(s->silence.memblock);

    pa_xfree(s->name);
    pa_xfree(s->driver);

    if (s->proplist)
        pa_proplist_free(s->proplist);

    pa_xfree(s);
}

void pa_sink_set_asyncmsgq(pa_sink *s, pa_asyncmsgq *q) {
    pa_sink_assert_ref(s);
    pa_assert(q);

    s->asyncmsgq = q;

    if (s->monitor_source)
        pa_source_set_asyncmsgq(s->monitor_source, q);
}

void pa_sink_set_rtpoll(pa_sink *s, pa_rtpoll *p) {
    pa_sink_assert_ref(s);
    pa_assert(p);

    s->rtpoll = p;
    if (s->monitor_source)
        pa_source_set_rtpoll(s->monitor_source, p);
}

int pa_sink_update_status(pa_sink*s) {
    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    if (s->state == PA_SINK_SUSPENDED)
        return 0;

    return sink_set_state(s, pa_sink_used_by(s) ? PA_SINK_RUNNING : PA_SINK_IDLE);
}

int pa_sink_suspend(pa_sink *s, pa_bool_t suspend) {
    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    if (suspend)
        return sink_set_state(s, PA_SINK_SUSPENDED);
    else
        return sink_set_state(s, pa_sink_used_by(s) ? PA_SINK_RUNNING : PA_SINK_IDLE);
}

void pa_sink_process_rewind(pa_sink *s, size_t nbytes) {
    pa_sink_input *i;
    void *state = NULL;
    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    if (nbytes <= 0)
        return;

    pa_log_debug("Processing rewind...");

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL))) {
        pa_sink_input_assert_ref(i);
        pa_sink_input_process_rewind(i, nbytes);
    }

    if (s->monitor_source && PA_SOURCE_OPENED(pa_source_get_state(s->monitor_source)))
        pa_source_process_rewind(s->monitor_source, nbytes);
}

static unsigned fill_mix_info(pa_sink *s, size_t *length, pa_mix_info *info, unsigned maxinfo) {
    pa_sink_input *i;
    unsigned n = 0;
    void *state = NULL;
    size_t mixlength = *length;

    pa_sink_assert_ref(s);
    pa_assert(info);

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)) && maxinfo > 0) {
        pa_sink_input_assert_ref(i);

        if (pa_sink_input_peek(i, *length, &info->chunk, &info->volume) < 0)
            continue;

        if (mixlength == 0 || info->chunk.length < mixlength)
            mixlength = info->chunk.length;

        if (pa_memblock_is_silence(info->chunk.memblock)) {
            pa_memblock_unref(info->chunk.memblock);
            continue;
        }

        info->userdata = pa_sink_input_ref(i);

        pa_assert(info->chunk.memblock);
        pa_assert(info->chunk.length > 0);

        info++;
        n++;
        maxinfo--;
    }

    if (mixlength > 0)
        *length = mixlength;

    return n;
}

static void inputs_drop(pa_sink *s, pa_mix_info *info, unsigned n, size_t length) {
    pa_sink_input *i;
    void *state = NULL;
    unsigned p = 0;
    unsigned n_unreffed = 0;

    pa_sink_assert_ref(s);

    /* We optimize for the case where the order of the inputs has not changed */

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL))) {
        unsigned j;
        pa_mix_info* m;

        pa_sink_input_assert_ref(i);

        m = NULL;

        /* Let's try to find the matching entry info the pa_mix_info array */
        for (j = 0; j < n; j ++) {

            if (info[p].userdata == i) {
                m = info + p;
                break;
            }

            p++;
            if (p >= n)
                p = 0;
        }

        /* Drop read data */
        pa_sink_input_drop(i, length);

        if (m) {
            pa_sink_input_unref(m->userdata);
            m->userdata = NULL;
            if (m->chunk.memblock)
                pa_memblock_unref(m->chunk.memblock);
            pa_memchunk_reset(&m->chunk);

            n_unreffed += 1;
        }
    }

    /* Now drop references to entries that are included in the
     * pa_mix_info array but don't exist anymore */

    if (n_unreffed < n) {
        for (; n > 0; info++, n--) {
            if (info->userdata)
                pa_sink_input_unref(info->userdata);
            if (info->chunk.memblock)
                pa_memblock_unref(info->chunk.memblock);
        }
    }
}

void pa_sink_render(pa_sink*s, size_t length, pa_memchunk *result) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t block_size_max;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_OPENED(s->thread_info.state));
    pa_assert(pa_frame_aligned(length, &s->sample_spec));
    pa_assert(result);

    pa_sink_ref(s);

    s->thread_info.rewind_nbytes = 0;

    if (length <= 0)
        length = pa_frame_align(MIX_BUFFER_LENGTH, &s->sample_spec);

    block_size_max = pa_mempool_block_size_max(s->core->mempool);
    if (length > block_size_max)
        length = pa_frame_align(block_size_max, &s->sample_spec);

    pa_assert(length > 0);

    n = s->thread_info.state == PA_SINK_RUNNING ? fill_mix_info(s, &length, info, MAX_MIX_CHANNELS) : 0;

    if (n == 0) {

        *result = s->silence;
        pa_memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

    } else if (n == 1) {
        pa_cvolume volume;

        *result = info[0].chunk;
        pa_memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

        pa_sw_cvolume_multiply(&volume, &s->thread_info.soft_volume, &info[0].volume);

        if (s->thread_info.soft_muted || !pa_cvolume_is_norm(&volume)) {
            pa_log("adjusting volume ");
            pa_memchunk_make_writable(result, 0);
            if (s->thread_info.soft_muted || pa_cvolume_is_muted(&volume))
                pa_silence_memchunk(result, &s->sample_spec);
            else
                pa_volume_memchunk(result, &s->sample_spec, &volume);
        }
    } else {
        void *ptr;
        result->memblock = pa_memblock_new(s->core->mempool, length);

        ptr = pa_memblock_acquire(result->memblock);
        result->length = pa_mix(info, n,
                                ptr, length,
                                &s->sample_spec,
                                &s->thread_info.soft_volume,
                                s->thread_info.soft_muted);
        pa_memblock_release(result->memblock);

        result->index = 0;
    }

    if (s->thread_info.state == PA_SINK_RUNNING)
        inputs_drop(s, info, n, result->length);

    if (s->monitor_source && PA_SOURCE_OPENED(pa_source_get_state(s->monitor_source)))
        pa_source_post(s->monitor_source, result);

    pa_sink_unref(s);
}

void pa_sink_render_into(pa_sink*s, pa_memchunk *target) {
    pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t length, block_size_max;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_OPENED(s->thread_info.state));
    pa_assert(target);
    pa_assert(target->memblock);
    pa_assert(target->length > 0);
    pa_assert(pa_frame_aligned(target->length, &s->sample_spec));

    pa_sink_ref(s);

    s->thread_info.rewind_nbytes = 0;

    length = target->length;
    block_size_max = pa_mempool_block_size_max(s->core->mempool);
    if (length > block_size_max)
        length = pa_frame_align(block_size_max, &s->sample_spec);

    n = s->thread_info.state == PA_SINK_RUNNING ? fill_mix_info(s, &length, info, MAX_MIX_CHANNELS) : 0;

    if (n == 0) {
        if (target->length > length)
            target->length = length;

        pa_silence_memchunk(target, &s->sample_spec);
    } else if (n == 1) {
        pa_cvolume volume;

        if (target->length > length)
            target->length = length;

        pa_sw_cvolume_multiply(&volume, &s->thread_info.soft_volume, &info[0].volume);

        if (s->thread_info.soft_muted || pa_cvolume_is_muted(&volume))
            pa_silence_memchunk(target, &s->sample_spec);
        else {
            pa_memchunk vchunk;

            vchunk = info[0].chunk;
            pa_memblock_ref(vchunk.memblock);

            if (vchunk.length > target->length)
                vchunk.length = target->length;

            if (!pa_cvolume_is_norm(&volume)) {
                pa_memchunk_make_writable(&vchunk, 0);
                pa_volume_memchunk(&vchunk, &s->sample_spec, &volume);
            }

            pa_memchunk_memcpy(target, &vchunk);
            pa_memblock_unref(vchunk.memblock);
        }

    } else {
        void *ptr;

        ptr = pa_memblock_acquire(target->memblock);

        target->length = pa_mix(info, n,
                                (uint8_t*) ptr + target->index, length,
                                &s->sample_spec,
                                &s->thread_info.soft_volume,
                                s->thread_info.soft_muted);

        pa_memblock_release(target->memblock);
    }

    if (s->thread_info.state == PA_SINK_RUNNING)
        inputs_drop(s, info, n, target->length);

    if (s->monitor_source && PA_SOURCE_OPENED(pa_source_get_state(s->monitor_source)))
        pa_source_post(s->monitor_source, target);

    pa_sink_unref(s);
}

void pa_sink_render_into_full(pa_sink *s, pa_memchunk *target) {
    pa_memchunk chunk;
    size_t l, d;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_OPENED(s->thread_info.state));
    pa_assert(target);
    pa_assert(target->memblock);
    pa_assert(target->length > 0);
    pa_assert(pa_frame_aligned(target->length, &s->sample_spec));

    pa_sink_ref(s);

    s->thread_info.rewind_nbytes = 0;

    l = target->length;
    d = 0;
    while (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;

        pa_sink_render_into(s, &chunk);

        d += chunk.length;
        l -= chunk.length;
    }

    pa_sink_unref(s);
}

void pa_sink_render_full(pa_sink *s, size_t length, pa_memchunk *result) {
    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_OPENED(s->thread_info.state));
    pa_assert(length > 0);
    pa_assert(pa_frame_aligned(length, &s->sample_spec));
    pa_assert(result);

    s->thread_info.rewind_nbytes = 0;

    /*** This needs optimization ***/

    result->index = 0;
    result->length = length;
    result->memblock = pa_memblock_new(s->core->mempool, length);

    pa_sink_render_into_full(s, result);
}

void pa_sink_skip(pa_sink *s, size_t length) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_OPENED(s->thread_info.state));
    pa_assert(length > 0);
    pa_assert(pa_frame_aligned(length, &s->sample_spec));

    s->thread_info.rewind_nbytes = 0;

    if (pa_source_used_by(s->monitor_source)) {
        pa_memchunk chunk;

        /* If something is connected to our monitor source, we have to
         * pass valid data to it */

        while (length > 0) {
            pa_sink_render(s, length, &chunk);
            pa_memblock_unref(chunk.memblock);

            pa_assert(chunk.length <= length);
            length -= chunk.length;
        }

    } else {
        /* Ok, noone cares about the rendered data, so let's not even render it */

        while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL))) {
            pa_sink_input_assert_ref(i);
            pa_sink_input_drop(i, length);
        }
    }
}

pa_usec_t pa_sink_get_latency(pa_sink *s) {
    pa_usec_t usec = 0;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    /* The returned value is supposed to be in the time domain of the sound card! */

    if (!PA_SINK_OPENED(s->state))
        return 0;

    if (s->get_latency)
        return s->get_latency(s);

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_LATENCY, &usec, 0, NULL) < 0)
        return 0;

    return usec;
}

void pa_sink_set_volume(pa_sink *s, const pa_cvolume *volume) {
    int changed;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));
    pa_assert(volume);

    changed = !pa_cvolume_equal(volume, &s->volume);
    s->volume = *volume;

    if (s->set_volume && s->set_volume(s) < 0)
        s->set_volume = NULL;

    if (!s->set_volume)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_VOLUME, pa_xnewdup(struct pa_cvolume, volume, 1), 0, NULL, pa_xfree);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

const pa_cvolume *pa_sink_get_volume(pa_sink *s) {
    struct pa_cvolume old_volume;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    old_volume = s->volume;

    if (s->get_volume && s->get_volume(s) < 0)
        s->get_volume = NULL;

    if (!s->get_volume && s->refresh_volume)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_VOLUME, &s->volume, 0, NULL);

    if (!pa_cvolume_equal(&old_volume, &s->volume))
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

    return &s->volume;
}

void pa_sink_set_mute(pa_sink *s, pa_bool_t mute) {
    int changed;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    changed = s->muted != mute;
    s->muted = mute;

    if (s->set_mute && s->set_mute(s) < 0)
        s->set_mute = NULL;

    if (!s->set_mute)
        pa_asyncmsgq_post(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_SET_MUTE, PA_UINT_TO_PTR(mute), 0, NULL, NULL);

    if (changed)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
}

pa_bool_t pa_sink_get_mute(pa_sink *s) {
    pa_bool_t old_muted;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    old_muted = s->muted;

    if (s->get_mute && s->get_mute(s) < 0)
        s->get_mute = NULL;

    if (!s->get_mute && s->refresh_mute)
        pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_MUTE, &s->muted, 0, NULL);

    if (old_muted != s->muted)
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);

    return s->muted;
}

void pa_sink_set_description(pa_sink *s, const char *description) {
    const char *old;
    pa_sink_assert_ref(s);

    if (!description && !pa_proplist_contains(s->proplist, PA_PROP_DEVICE_DESCRIPTION))
        return;

    old = pa_proplist_gets(s->proplist, PA_PROP_DEVICE_DESCRIPTION);

    if (old && description && !strcmp(old, description))
        return;

    if (description)
        pa_proplist_sets(s->proplist, PA_PROP_DEVICE_DESCRIPTION, description);
    else
        pa_proplist_unset(s->proplist, PA_PROP_DEVICE_DESCRIPTION);

    if (s->monitor_source) {
        char *n;

        n = pa_sprintf_malloc("Monitor Source of %s", description ? description : s->name);
        pa_source_set_description(s->monitor_source, n);
        pa_xfree(n);
    }

    if (PA_SINK_LINKED(s->state)) {
        pa_subscription_post(s->core, PA_SUBSCRIPTION_EVENT_SINK|PA_SUBSCRIPTION_EVENT_CHANGE, s->index);
        pa_hook_fire(&s->core->hooks[PA_CORE_HOOK_SINK_PROPLIST_CHANGED], s);
    }
}

unsigned pa_sink_linked_by(pa_sink *s) {
    unsigned ret;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    ret = pa_idxset_size(s->inputs);

    /* We add in the number of streams connected to us here. Please
     * not the asymmmetry to pa_sink_used_by()! */

    if (s->monitor_source)
        ret += pa_source_linked_by(s->monitor_source);

    return ret;
}

unsigned pa_sink_used_by(pa_sink *s) {
    unsigned ret;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    ret = pa_idxset_size(s->inputs);
    pa_assert(ret >= s->n_corked);

    /* Streams connected to our monitor source do not matter for
     * pa_sink_used_by()!.*/

    return ret - s->n_corked;
}

int pa_sink_process_msg(pa_msgobject *o, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_sink *s = PA_SINK(o);
    pa_sink_assert_ref(s);
    pa_assert(s->thread_info.state != PA_SINK_UNLINKED);

    switch ((pa_sink_message_t) code) {

        case PA_SINK_MESSAGE_ADD_INPUT: {
            pa_sink_input *i = PA_SINK_INPUT(userdata);

            pa_hashmap_put(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index), pa_sink_input_ref(i));

            /* Since the caller sleeps in pa_sink_input_put(), we can
             * safely access data outside of thread_info even though
             * it is mutable */

            if ((i->thread_info.sync_prev = i->sync_prev)) {
                pa_assert(i->sink == i->thread_info.sync_prev->sink);
                pa_assert(i->sync_prev->sync_next == i);
                i->thread_info.sync_prev->thread_info.sync_next = i;
            }

            if ((i->thread_info.sync_next = i->sync_next)) {
                pa_assert(i->sink == i->thread_info.sync_next->sink);
                pa_assert(i->sync_next->sync_prev == i);
                i->thread_info.sync_next->thread_info.sync_prev = i;
            }

            pa_sink_input_update_max_rewind(i, s->thread_info.max_rewind);

            pa_assert(!i->thread_info.attached);
            i->thread_info.attached = TRUE;

            if (i->attach)
                i->attach(i);

            /* If you change anything here, make sure to change the
             * ghost sink input handling a few lines down at
             * PA_SINK_MESSAGE_REMOVE_INPUT_AND_BUFFER, too. */

            pa_sink_invalidate_requested_latency(s);

            /* Make sure we're not rewound when the hw buffer is remixed and request a remix*/
            i->thread_info.ignore_rewind = TRUE;
            i->thread_info.since_underrun = 0;
            pa_sink_request_rewind(s, 0);

            return 0;
        }

        case PA_SINK_MESSAGE_REMOVE_INPUT: {
            pa_sink_input *i = PA_SINK_INPUT(userdata);

            /* If you change anything here, make sure to change the
             * sink input handling a few lines down at
             * PA_SINK_MESSAGE_REMOVE_INPUT_AND_BUFFER, too. */

            if (i->detach)
                i->detach(i);

            pa_assert(i->thread_info.attached);
            i->thread_info.attached = FALSE;

            /* Since the caller sleeps in pa_sink_input_unlink(),
             * we can safely access data outside of thread_info even
             * though it is mutable */

            pa_assert(!i->thread_info.sync_prev);
            pa_assert(!i->thread_info.sync_next);

            if (i->thread_info.sync_prev) {
                i->thread_info.sync_prev->thread_info.sync_next = i->thread_info.sync_prev->sync_next;
                i->thread_info.sync_prev = NULL;
            }

            if (i->thread_info.sync_next) {
                i->thread_info.sync_next->thread_info.sync_prev = i->thread_info.sync_next->sync_prev;
                i->thread_info.sync_next = NULL;
            }

            if (pa_hashmap_remove(s->thread_info.inputs, PA_UINT32_TO_PTR(i->index)))
                pa_sink_input_unref(i);

            pa_sink_invalidate_requested_latency(s);

            pa_sink_request_rewind(s, 0);

            return 0;
        }

        case PA_SINK_MESSAGE_REMOVE_INPUT_AND_BUFFER: {
            pa_sink_input_move_info *info = userdata;
            int volume_is_norm;

            /* We don't support moving synchronized streams. */
            pa_assert(!info->sink_input->sync_prev);
            pa_assert(!info->sink_input->sync_next);
            pa_assert(!info->sink_input->thread_info.sync_next);
            pa_assert(!info->sink_input->thread_info.sync_prev);

            if (info->sink_input->detach)
                info->sink_input->detach(info->sink_input);

            pa_assert(info->sink_input->thread_info.attached);
            info->sink_input->thread_info.attached = FALSE;
            pa_sink_invalidate_requested_latency(info->sink_input->sink);

            if (info->ghost_sink_input) {
                pa_assert(info->buffer_bytes > 0);
                pa_assert(info->buffer);

                volume_is_norm = pa_cvolume_is_norm(&info->sink_input->thread_info.volume);

                pa_log_debug("Buffering %lu bytes ...", (unsigned long) info->buffer_bytes);

                while (info->buffer_bytes > 0) {
                    pa_memchunk memchunk;
                    pa_cvolume volume;
                    size_t n;

                    if (pa_sink_input_peek(info->sink_input, info->buffer_bytes, &memchunk, &volume) < 0)
                        break;

                    n = memchunk.length > info->buffer_bytes ? info->buffer_bytes : memchunk.length;
                    pa_sink_input_drop(info->sink_input, n);
                    memchunk.length = n;

                    if (!volume_is_norm) {
                        pa_memchunk_make_writable(&memchunk, 0);
                        pa_volume_memchunk(&memchunk, &s->sample_spec, &volume);
                    }

                    if (pa_memblockq_push(info->buffer, &memchunk) < 0) {
                        pa_memblock_unref(memchunk.memblock);
                        break;
                    }

                    pa_memblock_unref(memchunk.memblock);
                    info->buffer_bytes -= n;
                }

                /* Add the remaining already resampled chunks to the buffer */
                pa_memblockq_splice(info->buffer, info->sink_input->thread_info.render_memblockq);

                pa_memblockq_sink_input_set_queue(info->ghost_sink_input, info->buffer);

                pa_log_debug("Buffered %lu bytes ...", (unsigned long) pa_memblockq_get_length(info->buffer));
            }

            /* Let's remove the sink input ...*/
            if (pa_hashmap_remove(s->thread_info.inputs, PA_UINT32_TO_PTR(info->sink_input->index)))
                pa_sink_input_unref(info->sink_input);

            /* .. and add the ghost sink input instead */
            if (info->ghost_sink_input) {
                pa_hashmap_put(s->thread_info.inputs, PA_UINT32_TO_PTR(info->ghost_sink_input->index), pa_sink_input_ref(info->ghost_sink_input));
                info->ghost_sink_input->thread_info.sync_prev = info->ghost_sink_input->thread_info.sync_next = NULL;

                pa_sink_input_update_max_rewind(info->ghost_sink_input, s->thread_info.max_rewind);

                pa_assert(!info->ghost_sink_input->thread_info.attached);
                info->ghost_sink_input->thread_info.attached = TRUE;

                if (info->ghost_sink_input->attach)
                    info->ghost_sink_input->attach(info->ghost_sink_input);

            }

            pa_sink_invalidate_requested_latency(s);

            pa_sink_request_rewind(s, 0);

            return 0;
        }

        case PA_SINK_MESSAGE_SET_VOLUME:
            s->thread_info.soft_volume = *((pa_cvolume*) userdata);

            pa_sink_request_rewind(s, 0);
            return 0;

        case PA_SINK_MESSAGE_SET_MUTE:
            s->thread_info.soft_muted = PA_PTR_TO_UINT(userdata);

            pa_sink_request_rewind(s, 0);
            return 0;

        case PA_SINK_MESSAGE_GET_VOLUME:
            *((pa_cvolume*) userdata) = s->thread_info.soft_volume;
            return 0;

        case PA_SINK_MESSAGE_GET_MUTE:
            *((pa_bool_t*) userdata) = s->thread_info.soft_muted;
            return 0;

        case PA_SINK_MESSAGE_SET_STATE:

            s->thread_info.state = PA_PTR_TO_UINT(userdata);
            return 0;

        case PA_SINK_MESSAGE_DETACH:

            /* We're detaching all our input streams so that the
             * asyncmsgq and rtpoll fields can be changed without
             * problems */
            pa_sink_detach_within_thread(s);
            return 0;

        case PA_SINK_MESSAGE_ATTACH:

            /* Reattach all streams */
            pa_sink_attach_within_thread(s);
            return 0;

        case PA_SINK_MESSAGE_GET_REQUESTED_LATENCY: {

            pa_usec_t *usec = userdata;
            *usec = pa_sink_get_requested_latency_within_thread(s);
            return 0;
        }

        case PA_SINK_MESSAGE_GET_LATENCY:
        case PA_SINK_MESSAGE_MAX:
            ;
    }

    return -1;
}

int pa_sink_suspend_all(pa_core *c, pa_bool_t suspend) {
    pa_sink *sink;
    uint32_t idx;
    int ret = 0;

    pa_core_assert_ref(c);

    for (sink = PA_SINK(pa_idxset_first(c->sinks, &idx)); sink; sink = PA_SINK(pa_idxset_next(c->sinks, &idx)))
        ret -= pa_sink_suspend(sink, suspend) < 0;

    return ret;
}

void pa_sink_detach(pa_sink *s) {
    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_DETACH, NULL, 0, NULL);
}

void pa_sink_attach(pa_sink *s) {
    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_ATTACH, NULL, 0, NULL);
}

void pa_sink_detach_within_thread(pa_sink *s) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->thread_info.state));

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)))
        if (i->detach)
            i->detach(i);

    if (s->monitor_source)
        pa_source_detach_within_thread(s->monitor_source);
}

void pa_sink_attach_within_thread(pa_sink *s) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->thread_info.state));

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)))
        if (i->attach)
            i->attach(i);

    if (s->monitor_source)
        pa_source_attach_within_thread(s->monitor_source);
}

void pa_sink_request_rewind(pa_sink*s, size_t nbytes) {
    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->thread_info.state));

    if (nbytes <= 0)
        nbytes = s->thread_info.max_rewind;

    nbytes = PA_MIN(nbytes, s->thread_info.max_rewind);

    if (nbytes <= s->thread_info.rewind_nbytes)
        return;

    s->thread_info.rewind_nbytes = nbytes;

    if (s->request_rewind)
        s->request_rewind(s);
}

pa_usec_t pa_sink_get_requested_latency_within_thread(pa_sink *s) {
    pa_usec_t result = (pa_usec_t) -1;
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);

    if (s->thread_info.requested_latency_valid)
        return s->thread_info.requested_latency;

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)))

        if (i->thread_info.requested_sink_latency != (pa_usec_t) -1 &&
            (result == (pa_usec_t) -1 || result > i->thread_info.requested_sink_latency))
            result = i->thread_info.requested_sink_latency;

    if (result != (pa_usec_t) -1) {
        if (s->max_latency > 0 && result > s->max_latency)
            result = s->max_latency;

        if (s->min_latency > 0 && result < s->min_latency)
            result = s->min_latency;
    }

    s->thread_info.requested_latency = result;
    s->thread_info.requested_latency_valid = TRUE;

    return result;
}

pa_usec_t pa_sink_get_requested_latency(pa_sink *s) {
    pa_usec_t usec = 0;

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->state));

    if (!PA_SINK_OPENED(s->state))
        return 0;

    if (pa_asyncmsgq_send(s->asyncmsgq, PA_MSGOBJECT(s), PA_SINK_MESSAGE_GET_REQUESTED_LATENCY, &usec, 0, NULL) < 0)
        return 0;

    return usec;
}

void pa_sink_set_max_rewind(pa_sink *s, size_t max_rewind) {
    pa_sink_input *i;
    void *state = NULL;

    pa_sink_assert_ref(s);

    if (max_rewind == s->thread_info.max_rewind)
        return;

    s->thread_info.max_rewind = max_rewind;

    while ((i = pa_hashmap_iterate(s->thread_info.inputs, &state, NULL)))
        pa_sink_input_update_max_rewind(i, s->thread_info.max_rewind);

    if (s->monitor_source)
        pa_source_set_max_rewind(s->monitor_source, s->thread_info.max_rewind);
}

void pa_sink_invalidate_requested_latency(pa_sink *s) {

    pa_sink_assert_ref(s);
    pa_assert(PA_SINK_LINKED(s->thread_info.state));

    if (!s->thread_info.requested_latency_valid)
        return;

    s->thread_info.requested_latency_valid = FALSE;

    if (s->update_requested_latency)
        s->update_requested_latency(s);
}
