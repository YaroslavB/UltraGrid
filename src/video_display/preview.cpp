/**
 * @file   video_display/preview.cpp
 * @author Martin Pulec     <pulec@cesnet.cz>
 * @author Martin Piatka    <piatka@cesnet.cz>
 */
/*
 * Copyright (c) 2014-2015 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#include "debug.h"
#include "lib_common.h"
#include "video.h"
#include "video_display.h"
#include "shared_mem_frame.hpp"
#include "video_codec.h"

#include <condition_variable>
#include <chrono>
#include <list>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <QSharedMemory>
#include <cmath>

using namespace std;

static constexpr int BUFFER_LEN = 5;
static constexpr unsigned int IN_QUEUE_MAX_BUFFER_LEN = 5;
static constexpr int SKIP_FIRST_N_FRAMES_IN_STREAM = 5;

struct state_preview_common {
        ~state_preview_common() {

        }

        struct video_desc display_desc;

        queue<struct video_frame *> incoming_queue;
        condition_variable in_queue_decremented_cv;

        mutex lock;
        condition_variable cv;

        QSharedMemory shared_mem;
        bool reconfiguring;
        size_t mem_size;
        codec_t frame_fmt;

        int scaledW, scaledH;
        int scaleF;
        int scaledW_pad;
        std::vector<unsigned char> scaled_frame;

        struct module *parent;
};

struct state_preview {
        shared_ptr<struct state_preview_common> common;
        struct video_desc desc;
};

static struct display *display_preview_fork(void *state)
{
        shared_ptr<struct state_preview_common> s = ((struct state_preview *)state)->common;
        struct display *out;
        char fmt[2 + sizeof(void *) * 2 + 1] = "";
        snprintf(fmt, sizeof fmt, "%p", state);

        int rc = initialize_video_display(s->parent,
                        "preview", fmt, 0, NULL, &out);
        if (rc == 0) return out; else return NULL;

        return out;
}

static void show_help(){
        printf("Preview display\n");
        printf("Internal use by GUI only\n");
}

static void *display_preview_init(struct module *parent, const char *fmt, unsigned int flags)
{
        struct state_preview *s;

        s = new state_preview();

        if (fmt && strlen(fmt) > 0) {
                if (isdigit(fmt[0])) { // fork
                        struct state_preview *orig;
                        sscanf(fmt, "%p", &orig);
                        s->common = orig->common;
                        return s;
                } else {
                        show_help();
                        return &display_init_noerr;
                }
        }
        s->common = shared_ptr<state_preview_common>(new state_preview_common());
        s->common->parent = parent;

        s->common->shared_mem.setKey("ultragrid_preview");
        s->common->mem_size = 4096;
        s->common->frame_fmt = RGB;
        if(s->common->shared_mem.create(s->common->mem_size) == false){
                /* Creating shared memory could fail because of shared memory
                 * left over after crash. Here we try to release such memory. */
                s->common->shared_mem.attach();
                s->common->shared_mem.detach();
                if(s->common->shared_mem.create(s->common->mem_size) == false){
                        fprintf(stderr, "Can't create shared memory!\n");
                        return &display_init_noerr;
                }
        }

        s->common->reconfiguring = false;
        s->common->shared_mem.lock();

        struct Shared_mem_frame *sframe = (Shared_mem_frame*) s->common->shared_mem.data();
        sframe->width = 20;
        sframe->height = 20;

        s->common->shared_mem.unlock();

        s->common->scaleF = 4;

        return s;
}

static void check_reconf(struct state_preview_common *s, struct video_desc desc)
{
        if (video_desc_eq(desc, s->display_desc))
                return;

        s->display_desc = desc;
        fprintf(stderr, "RECONFIGURED\n");
        /* We need to destroy the shared memory segment
         * and recreate it with a new size. To destroy it all processes
         * must detach it. We detach here and then wait until the GUI detaches */
        s->reconfiguring = true;

        const float target_width = 960;
        const float target_height = 540;

        float scale = ((desc.width / target_width) + (desc.height / target_height)) / 2.f;
        if(scale < 1)
                scale = 1;
        scale = std::round(scale);
        s->scaleF = (int) scale;

        s->scaledW = desc.width / s->scaleF;
        //OpenGL wants the width to be divisable by 4
        s->scaledW_pad = ((s->scaledW + 4 - 1) / 4) * 4;
        s->scaledH = desc.height / s->scaleF;
        s->scaled_frame.resize(get_bpp(desc.color_spec) * s->scaledW * s->scaledH);
        s->shared_mem.detach();
        s->mem_size = get_bpp(s->frame_fmt) * s->scaledW_pad * s->scaledH + sizeof(Shared_mem_frame);
}

static void display_preview_run(void *state)
{
        shared_ptr<struct state_preview_common> s = ((struct state_preview *)state)->common;
        int skipped = 0;

        while (1) {
                struct video_frame *frame;
                {
                        unique_lock<mutex> lg(s->lock);
                        s->cv.wait(lg, [s]{return s->incoming_queue.size() > 0;});
                        frame = s->incoming_queue.front();
                        s->incoming_queue.pop();
                        s->in_queue_decremented_cv.notify_one();
                }

                if (!frame) {
                        break;
                }

                if (skipped < SKIP_FIRST_N_FRAMES_IN_STREAM){
                        skipped++;
                        vf_free(frame);
                        continue;
                }

                check_reconf(s.get(), video_desc_from_frame(frame));

                if(s->reconfiguring){
                        if(s->shared_mem.attach()){
                                // Shared mem is still not detached by the GUI
                                s->shared_mem.detach();
                                vf_free(frame);
                                continue;
                        } else {
                                if(s->shared_mem.create(s->mem_size) == false){
                                        fprintf(stderr, "Can't create shared memory!\n");
                                }
                                s->shared_mem.lock();
                                struct Shared_mem_frame *sframe = (Shared_mem_frame*) s->shared_mem.data();
                                sframe->width = s->scaledW_pad;
                                sframe->height = s->scaledH;
                                s->reconfiguring = false;
                        }
                } else {
                        s->shared_mem.lock();
                }

                struct Shared_mem_frame *sframe = (Shared_mem_frame*) s->shared_mem.data();

                decoder_t dec = get_decoder_from_to(frame->color_spec, s->frame_fmt, true);

                int src_line_len = vc_get_linesize(s->display_desc.width, frame->color_spec);
                int block_size = get_pf_block_size(frame->color_spec);
                int dst = 0;
                for(unsigned y = 0; y < s->display_desc.height; y += s->scaleF){
                        for(int x = 0; x + s->scaleF * block_size <= src_line_len; x += s->scaleF * block_size){
                                memcpy(s->scaled_frame.data() + dst, frame->tiles[0].data + y*src_line_len + x, block_size);
                                dst += block_size;
                        }
                }
                int dst_line_len_pad = vc_get_linesize(s->scaledW_pad, s->frame_fmt);
                int dst_line_len = vc_get_linesize(s->scaledW, s->frame_fmt);
                src_line_len = vc_get_linesize(s->scaledW, frame->color_spec);
                for(int i = 0; i < s->scaledH; i++){
                        dec(sframe->pixels + dst_line_len_pad * i,
                                        s->scaled_frame.data() + src_line_len * i,
                                        dst_line_len,
                                        0, 8, 16);
                }

                s->shared_mem.unlock();
                vf_free(frame);
        }
}

static void display_preview_done(void *state)
{
        struct state_preview *s = (struct state_preview *)state;
        delete s;
}

static struct video_frame *display_preview_getf(void *state)
{
        struct state_preview *s = (struct state_preview *)state;

        return vf_alloc_desc_data(s->desc);
}

static int display_preview_putf(void *state, struct video_frame *frame, int flags)
{
        shared_ptr<struct state_preview_common> s = ((struct state_preview *)state)->common;

        if (flags == PUTF_DISCARD) {
                vf_free(frame);
        } else {
                unique_lock<mutex> lg(s->lock);
                if (s->incoming_queue.size() >= IN_QUEUE_MAX_BUFFER_LEN) {
                        fprintf(stderr, "Multiplier: queue full!\n");
                }
                if (flags == PUTF_NONBLOCK && s->incoming_queue.size() >= IN_QUEUE_MAX_BUFFER_LEN) {
                        vf_free(frame);
                        return 1;
                }
                s->in_queue_decremented_cv.wait(lg, [s]{return s->incoming_queue.size() < IN_QUEUE_MAX_BUFFER_LEN;});
                s->incoming_queue.push(frame);
                lg.unlock();
                s->cv.notify_one();
        }

        return 0;
}

static int display_preview_get_property(void *state, int property, void *val, size_t *len)
{
        UNUSED(state);
        codec_t codecs[] = {UYVY, RGBA, RGB};
        enum interlacing_t supported_il_modes[] = {PROGRESSIVE, INTERLACED_MERGED, SEGMENTED_FRAME};
        int rgb_shift[] = {0, 8, 16};

        switch (property) {
                case DISPLAY_PROPERTY_CODECS:
                        if(sizeof(codecs) <= *len) {
                                memcpy(val, codecs, sizeof(codecs));
                        } else {
                                return FALSE;
                        }

                        *len = sizeof(codecs);
                        break;
                case DISPLAY_PROPERTY_RGB_SHIFT:
                        if(sizeof(rgb_shift) > *len) {
                                return FALSE;
                        }
                        memcpy(val, rgb_shift, sizeof(rgb_shift));
                        *len = sizeof(rgb_shift);
                        break;
                case DISPLAY_PROPERTY_BUF_PITCH:
                        *(int *) val = PITCH_DEFAULT;
                        *len = sizeof(int);
                        break;
                case DISPLAY_PROPERTY_SUPPORTED_IL_MODES:
                        if(sizeof(supported_il_modes) <= *len) {
                                memcpy(val, supported_il_modes, sizeof(supported_il_modes));
                        } else {
                                return FALSE;
                        }
                        *len = sizeof(supported_il_modes);
                        break;
                default:
                        return FALSE;
        }
        return TRUE;
}

static int display_preview_reconfigure(void *state, struct video_desc desc)
{
        struct state_preview *s = (struct state_preview *) state;

        s->desc = desc;

        return 1;
}

static void display_preview_put_audio_frame(void *state, struct audio_frame *frame)
{
        UNUSED(state);
        UNUSED(frame);
}

static int display_preview_reconfigure_audio(void *state, int quant_samples, int channels,
                int sample_rate)
{
        UNUSED(state);
        UNUSED(quant_samples);
        UNUSED(channels);
        UNUSED(sample_rate);

        return FALSE;
}

static const struct video_display_info display_preview_info = {
        [](struct device_info **available_cards, int *count) {
                *available_cards = nullptr;
                *count = 0;
        },
        display_preview_init,
        display_preview_run,
        display_preview_done,
        display_preview_getf,
        display_preview_putf,
        display_preview_reconfigure,
        display_preview_get_property,
        display_preview_put_audio_frame,
        display_preview_reconfigure_audio,
};

REGISTER_MODULE(preview, &display_preview_info, LIBRARY_CLASS_VIDEO_DISPLAY, VIDEO_DISPLAY_ABI_VERSION);

