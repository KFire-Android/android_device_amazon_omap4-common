/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AUDIO_ROUTE_H
#define AUDIO_ROUTE_H

struct mixer_state {
    struct mixer_ctl *ctl;
    int old_value;
    int new_value;
    int reset_value;
};

struct mixer_setting {
    struct mixer_ctl *ctl;
    int value;
};

struct mixer_path {
    char *name;
    unsigned int size;
    unsigned int length;
    struct mixer_setting *setting;
};

struct audio_route {
    unsigned int num_mixer_ctls;
    struct mixer_state *mixer_state;

    unsigned int mixer_path_size;
    unsigned int num_mixer_paths;
    struct mixer_path *mixer_path;
};

struct config_parse_state {
    struct audio_route *ar;
    struct mixer_path *path;
    int level;
};

/* Initialises and frees the audio routes */
struct audio_route *audio_route_init(struct mixer *mixer);
void audio_route_free(struct audio_route *ar);

/* Applies an audio route path by name */
void audio_route_apply_path(struct audio_route *ar, const char *name);

/* Resets the mixer back to its initial state */
void reset_mixer_state(struct audio_route *ar);

/* Updates the mixer with any changed values */
void update_mixer_state(struct audio_route *ar);

#endif
