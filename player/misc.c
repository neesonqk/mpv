/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <stdbool.h>
#include <assert.h>

#include "config.h"
#include "mpv_talloc.h"

#include "osdep/io.h"
#include "osdep/timer.h"
#include "osdep/threads.h"

#include "common/msg.h"
#include "options/options.h"
#include "options/m_property.h"
#include "options/m_config.h"
#include "common/common.h"
#include "common/global.h"
#include "common/encode.h"
#include "common/playlist.h"
#include "input/input.h"

#include "audio/out/ao.h"
#include "demux/demux.h"
#include "stream/stream.h"
#include "video/out/vo.h"

#include "core.h"
#include "command.h"

double rel_time_to_abs(struct MPContext *mpctx, struct m_rel_time t)
{
    double length = get_time_length(mpctx);
    switch (t.type) {
    case REL_TIME_ABSOLUTE:
        return t.pos;
    case REL_TIME_RELATIVE:
        if (t.pos >= 0) {
            return t.pos;
        } else {
            if (length >= 0)
                return MPMAX(length + t.pos, 0.0);
        }
        break;
    case REL_TIME_PERCENT:
        if (length >= 0)
            return length * (t.pos / 100.0);
        break;
    case REL_TIME_CHAPTER:
        if (chapter_start_time(mpctx, t.pos) != MP_NOPTS_VALUE)
            return chapter_start_time(mpctx, t.pos);
        break;
    }
    return MP_NOPTS_VALUE;
}

double get_play_end_pts(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    double end = MP_NOPTS_VALUE;
    if (opts->play_end.type) {
        end = rel_time_to_abs(mpctx, opts->play_end);
    } else if (opts->play_length.type) {
        double start = rel_time_to_abs(mpctx, opts->play_start);
        if (start == MP_NOPTS_VALUE)
            start = 0;
        double length = rel_time_to_abs(mpctx, opts->play_length);
        if (length != MP_NOPTS_VALUE)
            end = start + length;
    }
    if (opts->chapterrange[1] > 0) {
        double cend = chapter_start_time(mpctx, opts->chapterrange[1]);
        if (cend != MP_NOPTS_VALUE && (end == MP_NOPTS_VALUE || cend < end))
            end = cend;
    }
    if (mpctx->ab_loop_clip && opts->ab_loop[1] != MP_NOPTS_VALUE &&
        opts->ab_loop[1] > opts->ab_loop[0])
    {
        if (end == MP_NOPTS_VALUE || end > opts->ab_loop[1])
            end = opts->ab_loop[1];
    }
    return end;
}

double get_track_seek_offset(struct MPContext *mpctx, struct track *track)
{
    struct MPOpts *opts = mpctx->opts;
    if (track->selected) {
        if (track->type == STREAM_AUDIO)
            return -opts->audio_delay;
        if (track->type == STREAM_SUB)
            return -opts->sub_delay;
    }
    return 0;
}

float mp_get_cache_percent(struct MPContext *mpctx)
{
    struct stream_cache_info info = {0};
    if (mpctx->demuxer)
        demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_CACHE_INFO, &info);
    if (info.size > 0 && info.fill >= 0)
        return info.fill / (info.size / 100.0);
    return -1;
}

bool mp_get_cache_idle(struct MPContext *mpctx)
{
    struct stream_cache_info info = {0};
    if (mpctx->demuxer)
        demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_CACHE_INFO, &info);
    return info.idle;
}

void update_vo_playback_state(struct MPContext *mpctx)
{
    if (mpctx->video_out && mpctx->video_out->config_ok) {
        struct voctrl_playback_state oldstate = mpctx->vo_playback_state;
        struct voctrl_playback_state newstate = {
            .taskbar_progress = mpctx->opts->vo->taskbar_progress,
            .playing = mpctx->playing,
            .paused = mpctx->paused,
            .percent_pos = get_percent_pos(mpctx),
        };

        if (oldstate.taskbar_progress != newstate.taskbar_progress ||
            oldstate.playing != newstate.playing ||
            oldstate.paused != newstate.paused ||
            oldstate.percent_pos != newstate.percent_pos)
        {
            // Don't update progress bar if it was and still is hidden
            if ((oldstate.playing && oldstate.taskbar_progress) ||
                (newstate.playing && newstate.taskbar_progress))
            {
                vo_control_async(mpctx->video_out,
                                 VOCTRL_UPDATE_PLAYBACK_STATE, &newstate);
            }
            mpctx->vo_playback_state = newstate;
        }
    } else {
        mpctx->vo_playback_state = (struct voctrl_playback_state){ 0 };
    }
}

void update_window_title(struct MPContext *mpctx, bool force)
{
    if (!mpctx->video_out && !mpctx->ao) {
        talloc_free(mpctx->last_window_title);
        mpctx->last_window_title = NULL;
        return;
    }
    char *title = mp_property_expand_string(mpctx, mpctx->opts->wintitle);
    if (!mpctx->last_window_title || force ||
        strcmp(title, mpctx->last_window_title) != 0)
    {
        talloc_free(mpctx->last_window_title);
        mpctx->last_window_title = talloc_steal(mpctx, title);

        if (mpctx->video_out)
            vo_control(mpctx->video_out, VOCTRL_UPDATE_WINDOW_TITLE, title);

        if (mpctx->ao) {
            ao_control(mpctx->ao, AOCONTROL_UPDATE_STREAM_TITLE, title);
        }
    } else {
        talloc_free(title);
    }
}

void error_on_track(struct MPContext *mpctx, struct track *track)
{
    if (!track || !track->selected)
        return;
    mp_deselect_track(mpctx, track);
    if (track->type == STREAM_AUDIO)
        MP_INFO(mpctx, "Audio: no audio\n");
    if (track->type == STREAM_VIDEO)
        MP_INFO(mpctx, "Video: no video\n");
    if (mpctx->opts->stop_playback_on_init_failure ||
        !(mpctx->vo_chain || mpctx->ao_chain))
    {
        if (!mpctx->stop_play)
            mpctx->stop_play = PT_ERROR;
        if (mpctx->error_playing >= 0)
            mpctx->error_playing = MPV_ERROR_NOTHING_TO_PLAY;
    }
    mp_wakeup_core(mpctx);
}

int stream_dump(struct MPContext *mpctx, const char *source_filename)
{
    struct MPOpts *opts = mpctx->opts;
    stream_t *stream = stream_open(source_filename, mpctx->global);
    if (!stream)
        return -1;

    int64_t size = stream_get_size(stream);

    stream_set_capture_file(stream, opts->stream_dump);

    while (mpctx->stop_play == KEEP_PLAYING && !stream->eof) {
        if (!opts->quiet && ((stream->pos / (1024 * 1024)) % 2) == 1) {
            uint64_t pos = stream->pos;
            MP_MSG(mpctx, MSGL_STATUS, "Dumping %lld/%lld...",
                   (long long int)pos, (long long int)size);
        }
        stream_fill_buffer(stream);
        mp_wakeup_core(mpctx); // don't actually sleep
        mp_idle(mpctx); // but process input
    }

    free_stream(stream);
    return 0;
}

void merge_playlist_files(struct playlist *pl)
{
    if (!pl->first)
        return;
    char *edl = talloc_strdup(NULL, "edl://");
    for (struct playlist_entry *e = pl->first; e; e = e->next) {
        if (e != pl->first)
            edl = talloc_strdup_append_buffer(edl, ";");
        // Escape if needed
        if (e->filename[strcspn(e->filename, "=%,;\n")] ||
            bstr_strip(bstr0(e->filename)).len != strlen(e->filename))
        {
            // %length%
            edl = talloc_asprintf_append_buffer(edl, "%%%zd%%", strlen(e->filename));
        }
        edl = talloc_strdup_append_buffer(edl, e->filename);
    }
    playlist_clear(pl);
    playlist_add_file(pl, edl);
    talloc_free(edl);
}
