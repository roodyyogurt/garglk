// Copyright (C) 2006-2009 by Tor Andersson, Lorenzo Marcantonio.
// Copyright (C) 2010 by Ben Cressey, Chris Spiegel.
//
// This file is part of Gargoyle.
//
// Gargoyle is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// Gargoyle is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Gargoyle; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

// SDL support donated by Lorenzo Marcantonio

#ifdef _WIN32
#define SDL_MAIN_HANDLED
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>
#include <SDL_mixer.h>

#include "glk.h"
#include "garglk.h"

#include "gi_blorb.h"

#define giblorb_ID_MOD  (giblorb_make_id('M', 'O', 'D', ' '))
#define giblorb_ID_OGG  (giblorb_make_id('O', 'G', 'G', 'V'))
#define giblorb_ID_FORM (giblorb_make_id('F', 'O', 'R', 'M'))
#define giblorb_ID_AIFF (giblorb_make_id('A', 'I', 'F', 'F'))

// non-standard types
#define giblorb_ID_MP3  (giblorb_make_id('M', 'P', '3', ' '))
#define giblorb_ID_WAVE (giblorb_make_id('W', 'A', 'V', 'E'))
#define giblorb_ID_MIDI (giblorb_make_id('M', 'I', 'D', 'I'))

#define SDL_CHANNELS 64
#define GLK_MAXVOLUME 0x10000
#define FADE_GRANULARITY 100

#define GLK_VOLUME_TO_SDL_VOLUME(x) ((x) < GLK_MAXVOLUME ? (std::round(std::pow(((double)x) / GLK_MAXVOLUME, std::log(4)) * MIX_MAX_VOLUME)) : (MIX_MAX_VOLUME))

enum { CHANNEL_IDLE, CHANNEL_SOUND, CHANNEL_MUSIC };

struct glk_schannel_struct {
    glui32 rock;

    Mix_Chunk *sample;
    Mix_Music *music;

    SDL_RWops *sdl_rwops;
    std::vector<unsigned char> sdl_memory;
    int sdl_channel;

    int resid; // for notifies
    int status;
    int channel;
    int volume;
    glui32 loop;
    int notify;

    bool paused;

    // for volume fades
    int volume_notify;
    int volume_timeout;
    int target_volume;
    double float_volume;
    double volume_delta;
    SDL_TimerID timer;

    gidispatch_rock_t disprock;
    channel_t *chain_next, *chain_prev;
};

static channel_t *gli_channellist = nullptr;
static std::array<channel_t *, SDL_CHANNELS> sound_channels;
static channel_t *music_channel;

static schanid_t gli_bleep_channel;

static const int FREE = 1;
static const int BUSY = 2;

gidispatch_rock_t gli_sound_get_channel_disprock(const channel_t *chan)
{
    return chan->disprock;
}

void gli_initialize_sound()
{
    if (gli_conf_sound) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) == -1) {
            gli_strict_warning("SDL init failed\n");
            gli_strict_warning(SDL_GetError());
            gli_conf_sound = false;
            return;
        }
        // MixInit?

        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) == -1) {
            gli_strict_warning("SDL Mixer init failed\n");
            gli_strict_warning(Mix_GetError());
            gli_conf_sound = false;
            return;
        }

        int channels = Mix_AllocateChannels(SDL_CHANNELS);
        Mix_GroupChannels(0, channels - 1, FREE);
        Mix_ChannelFinished(nullptr);
    }
}

schanid_t glk_schannel_create(glui32 rock)
{
    return glk_schannel_create_ext(rock, GLK_MAXVOLUME);
}

schanid_t glk_schannel_create_ext(glui32 rock, glui32 volume)
{
    channel_t *chan;

    if (!gli_conf_sound) {
        return nullptr;
    }
    chan = new channel_t;

    chan->rock = rock;
    chan->status = CHANNEL_IDLE;
    chan->volume = GLK_VOLUME_TO_SDL_VOLUME(volume);
    chan->resid = 0;
    chan->loop = 0;
    chan->notify = 0;
    chan->sdl_rwops = nullptr;
    chan->sample = nullptr;
    chan->paused = false;
    chan->sdl_channel = -1;
    chan->music = nullptr;

    chan->volume_notify = 0;
    chan->volume_timeout = 0;
    chan->target_volume = 0;
    chan->float_volume = 0;
    chan->volume_delta = 0;
    chan->timer = 0;

    chan->chain_prev = nullptr;
    chan->chain_next = gli_channellist;
    gli_channellist = chan;
    if (chan->chain_next) {
        chan->chain_next->chain_prev = chan;
    }

    if (gli_register_obj) {
        chan->disprock = (*gli_register_obj)(chan, gidisp_Class_Schannel);
    } else {
        chan->disprock.ptr = nullptr;
    }

    return chan;
}

static void cleanup_channel(schanid_t chan)
{
    if (chan->sdl_rwops) {
        SDL_FreeRW(chan->sdl_rwops);
        chan->sdl_rwops = nullptr;
    }

    chan->sdl_memory.clear();

    switch (chan->status) {
    case CHANNEL_SOUND:
        if (chan->sample) {
            Mix_FreeChunk(chan->sample);
        }
        if (chan->sdl_channel >= 0) {
            Mix_GroupChannel(chan->sdl_channel, FREE);
            sound_channels[chan->sdl_channel] = nullptr;
        }
        break;
    case CHANNEL_MUSIC:
        if (chan->music) {
            Mix_FreeMusic(chan->music);
            music_channel = nullptr;
        }
        break;
    }
    chan->status = CHANNEL_IDLE;
    chan->sdl_channel = -1;
    chan->music = nullptr;

    if (chan->timer) {
        SDL_RemoveTimer(chan->timer);
    }

    chan->timer = 0;
}

void glk_schannel_destroy(schanid_t chan)
{
    channel_t *prev, *next;

    if (!chan) {
        gli_strict_warning("schannel_destroy: invalid id.");
        return;
    }

    glk_schannel_stop(chan);
    cleanup_channel(chan);
    if (gli_unregister_obj) {
        (*gli_unregister_obj)(chan, gidisp_Class_Schannel, chan->disprock);
    }

    prev = chan->chain_prev;
    next = chan->chain_next;
    chan->chain_prev = nullptr;
    chan->chain_next = nullptr;

    if (prev) {
        prev->chain_next = next;
    } else {
        gli_channellist = next;
    }

    if (next) {
        next->chain_prev = prev;
    }

    delete chan;
}

schanid_t glk_schannel_iterate(schanid_t chan, glui32 *rock)
{
    if (!chan) {
        chan = gli_channellist;
    } else {
        chan = chan->chain_next;
    }

    if (chan) {
        if (rock) {
            *rock = chan->rock;
        }
        return chan;
    }

    if (rock) {
        *rock = 0;
    }
    return nullptr;
}

glui32 glk_schannel_get_rock(schanid_t chan)
{
    if (!chan) {
        gli_strict_warning("schannel_get_rock: invalid id.");
        return 0;
    }
    return chan->rock;
}

glui32 glk_schannel_play(schanid_t chan, glui32 snd)
{
    return glk_schannel_play_ext(chan, snd, 1, 0);
}

glui32 glk_schannel_play_multi(schanid_t *chanarray, glui32 chancount,
        glui32 *sndarray, glui32 soundcount, glui32 notify)
{
    int successes = 0;

    for (glui32 i = 0; i < chancount; i++) {
        successes += glk_schannel_play_ext(chanarray[i], sndarray[i], 1, notify);
    }

    return successes;
}

void glk_sound_load_hint(glui32 snd, glui32 flag)
{
    // nop
}

// Make an incremental volume change when the fade timer fires
Uint32 volume_timer_callback(Uint32 interval, void *param)
{
    schanid_t chan = static_cast<schanid_t>(param);

    if (!chan) {
        gli_strict_warning("volume_timer_callback: invalid channel.");
        return 0;
    }

    chan->float_volume += chan->volume_delta;

    if (chan->float_volume < 0) {
        chan->float_volume = 0;
    }

    if (static_cast<int>(chan->float_volume) != chan->volume) {
        chan->volume = static_cast<int>(chan->float_volume);

        if (chan->status == CHANNEL_SOUND) {
            Mix_Volume(chan->sdl_channel, chan->volume);
        } else if (chan->status == CHANNEL_MUSIC) {
            Mix_VolumeMusic(chan->volume);
        }
    }

    chan->volume_timeout--;

    // If the timer has fired FADE_GRANULARITY times, kill it
    if (chan->volume_timeout <= 0) {
        if (chan->volume_notify) {
            gli_event_store(evtype_VolumeNotify, nullptr,
                0, chan->volume_notify);
            gli_notification_waiting();
        }

        if (!chan->timer) {
            gli_strict_warning("volume_timer_callback: invalid timer.");
            return 0;
        }
        SDL_RemoveTimer(chan->timer);
        chan->timer = 0;

        if (chan->volume != chan->target_volume) {
            chan->volume = chan->target_volume;
            if (chan->status == CHANNEL_SOUND) {
                Mix_Volume(chan->sdl_channel, chan->volume);
            } else if (chan->status == CHANNEL_MUSIC) {
                Mix_VolumeMusic(chan->volume);
            }
        }
        return 0;
    }

    return interval;
}

// Start a fade timer
void init_fade(schanid_t chan, int glk_volume, int duration, int notify)
{
    if (!chan) {
        gli_strict_warning("init_fade: invalid channel.");
        return;
    }

    chan->volume_notify = notify;

    chan->target_volume = GLK_VOLUME_TO_SDL_VOLUME(glk_volume);

    chan->float_volume = static_cast<double>(chan->volume);
    chan->volume_delta = static_cast<double>(chan->target_volume - chan->volume) / FADE_GRANULARITY;

    chan->volume_timeout = FADE_GRANULARITY;

    if (chan->timer) {
        SDL_RemoveTimer(chan->timer);
    }

    chan->timer = SDL_AddTimer(static_cast<Uint32>(duration / FADE_GRANULARITY), volume_timer_callback, chan);

    if (!chan->timer) {
        gli_strict_warning("init_fade: failed to create volume change timer.");
        return;
    }
}

void glk_schannel_set_volume(schanid_t chan, glui32 vol)
{
    glk_schannel_set_volume_ext(chan, vol, 0, 0);
}

void glk_schannel_set_volume_ext(schanid_t chan, glui32 glk_volume,
        glui32 duration, glui32 notify)
{
    if (!chan) {
        gli_strict_warning("schannel_set_volume: invalid id.");
        return;
    }

    if (!duration) {

        chan->volume = GLK_VOLUME_TO_SDL_VOLUME(glk_volume);

        switch (chan->status) {
        case CHANNEL_IDLE:
            break;
        case CHANNEL_SOUND:
            Mix_Volume(chan->sdl_channel, chan->volume);
            break;
        case CHANNEL_MUSIC:
            Mix_VolumeMusic(chan->volume);
            break;
        }
    } else {
        init_fade(chan, glk_volume, duration, notify);
    }
}

// Notify the music channel completion
static void music_completion_callback()
{
    if (!music_channel) {
        gli_strict_warning("music callback failed");
        return;
    } else {
        gli_event_store(evtype_SoundNotify, nullptr, music_channel->resid,
            music_channel->notify);
        gli_notification_waiting();
    }
    cleanup_channel(music_channel);
}

// Notify the sound channel completion
static void sound_completion_callback(int chan)
{
    channel_t *sound_channel = sound_channels[chan];

    if (!sound_channel) {
        gli_strict_warning("sound completion callback called with invalid channel");
        return;
    }

    if (sound_channel->notify) {
        gli_event_store(evtype_SoundNotify, nullptr,
            sound_channel->resid, sound_channel->notify);
        gli_notification_waiting();
    }
    Mix_ChannelFinished(nullptr);
    cleanup_channel(sound_channel);
    sound_channels[chan] = nullptr;
    return;
}

static int detect_format(const std::vector<unsigned char> &buf)
{
    const std::vector<std::pair<std::pair<long, std::vector<std::string>>, unsigned long>> formats = {
        // AIFF
        {{0, {"FORM"}}, giblorb_ID_FORM},

        // WAVE
        {{0, {"WAVE", "RIFF"}}, giblorb_ID_WAVE},

        // midi
        {{0, {"MThd"}}, giblorb_ID_MIDI},

        // s3m
        {{44, {"SCRM"}}, giblorb_ID_MOD},

        // XM
        {{0, {"Extended Module: "}}, giblorb_ID_MOD},

        // IT
        {{0, {"IMPM"}}, giblorb_ID_MOD},

        // MOD
        {{1080, {"4CHN", "6CHN", "8CHN",
                 "16CN", "32CN", "M.K.",
                 "M!K!", "FLT4", "CD81",
                 "OKTA", "    "}}, giblorb_ID_MOD},

        // ogg
        {{0, {"OggS"}}, giblorb_ID_OGG},

        // mp3
        {{0, {"\377\372"}}, giblorb_ID_MP3},
    };

    for (const auto &entry : formats) {
        auto offset = entry.first.first;
        auto magics = entry.first.second;
        auto format = entry.second;

        if (std::any_of(magics.begin(), magics.end(), [&offset, &buf](const auto &magic) {
            return offset + magic.size() < buf.size() &&
                    std::memcmp(buf.data() + offset, magic.data(), magic.size()) == 0;
        }))
        {
            return format;
        }
    }

    return 0;
}

static int load_bleep_resource(glui32 snd, std::vector<unsigned char> &buf)
{
    if (snd != 1 && snd != 2) {
        return 0;
    }

    const auto &bleep = gli_bleeps.at(snd);
    buf.assign(bleep.begin(), bleep.end());

    return detect_format(buf);
}

static glui32 load_sound_resource(glui32 snd, std::vector<unsigned char> &buf)
{
    long len;

    if (giblorb_get_resource_map() == nullptr) {
        std::string name;

        name = gli_workdir + "/SND" + std::to_string(snd);

        auto file = garglk::unique(std::fopen(name.c_str(), "rb"), fclose);
        if (!file) {
            return 0;
        }

        std::fseek(file.get(), 0, SEEK_END);

        try {
            buf.resize(std::ftell(file.get()));
        } catch (const std::bad_alloc &) {
            return 0;
        }

        std::rewind(file.get());
        if (std::fread(buf.data(), 1, buf.size(), file.get()) != buf.size() && !std::feof(file.get())) {
            return 0;
        }

        return detect_format(buf);
    } else {
        std::FILE *file;
        glui32 type;
        long pos;

        giblorb_get_resource(giblorb_ID_Snd, snd, &file, &pos, &len, &type);
        if (!file) {
            return 0;
        }

        try {
            buf.resize(len);
        } catch (const std::bad_alloc &) {
            return 0;
        }

        std::fseek(file, pos, SEEK_SET);
        if (std::fread(buf.data(), 1, buf.size(), file) != buf.size() && !std::feof(file)) {
            return 0;
        }

        return type;
    }
}

// Start a sound channel
static glui32 play_sound(schanid_t chan)
{
    int loop;
    SDL_LockAudio();
    chan->status = CHANNEL_SOUND;
    chan->sdl_channel = Mix_GroupAvailable(FREE);
    Mix_GroupChannel(chan->sdl_channel, BUSY);
    SDL_UnlockAudio();
    chan->sample = Mix_LoadWAV_RW(chan->sdl_rwops, false);
    if (chan->sdl_channel < 0) {
        gli_strict_warning("No available sound channels");
    }
    if (chan->sdl_channel >= 0 && chan->sample) {
        SDL_LockAudio();
        sound_channels[chan->sdl_channel] = chan;
        SDL_UnlockAudio();
        Mix_Volume(chan->sdl_channel, chan->volume);
        Mix_ChannelFinished(&sound_completion_callback);
        loop = chan->loop - 1;
        if (loop < -1) {
            loop = -1;
        }
        if (Mix_PlayChannel(chan->sdl_channel, chan->sample, loop) >= 0) {
            return 1;
        }
    }
    gli_strict_warning("play sound failed");
    gli_strict_warning(Mix_GetError());
    SDL_LockAudio();
    cleanup_channel(chan);
    SDL_UnlockAudio();
    return 0;
}

// Start a mod music channel
static glui32 play_mod(schanid_t chan, long len)
{
    std::FILE *file;
    const char *tempdir;
    bool music_busy;
    int loop;

    if (chan == nullptr) {
        gli_strict_warning("MOD player called with an invalid channel!");
        return 0;
    }

    music_busy = Mix_PlayingMusic();

    if (music_busy) {
        // We already checked for music playing on *this* channel
        // in glk_schannel_play_ext

        gli_strict_warning("MOD player already in use on another channel!");
        return 0;
    }

    chan->status = CHANNEL_MUSIC;
    // The fscking mikmod lib want to read the mod only from disk!
    tempdir = std::getenv("TMPDIR");
    if (tempdir == nullptr) {
        tempdir = std::getenv("TEMP");
        if (tempdir == nullptr) {
            tempdir = std::getenv("TMP");
            if (tempdir == nullptr) {
                tempdir = ".";
            }
        }
    }

    // allocate size of string tempdir + "XXXXXX' + terminator
    std::vector<char> tn(std::strlen(tempdir) + 7);
    std::sprintf(tn.data(), "%sXXXXXX", tempdir);
    int fd;
    fd = mkstemp(tn.data());
    if (fd == -1) {
        gli_strict_warning("play mod failed: could not create temp file");
        return 0;
    }
    file = fdopen(fd, "wb");
    std::fwrite(chan->sdl_memory.data(), 1, len, file);
    std::fclose(file);
    chan->music = Mix_LoadMUS(tn.data());
    std::remove(tn.data());
    if (chan->music) {
        SDL_LockAudio();
        music_channel = chan;
        SDL_UnlockAudio();
        Mix_VolumeMusic(chan->volume);
        Mix_HookMusicFinished(&music_completion_callback);
        loop = chan->loop;
        if (loop < -1) {
            loop = -1;
        }
        if (Mix_PlayMusic(chan->music, loop) >= 0) {
            return 1;
        }
    }
    gli_strict_warning("play mod failed");
    gli_strict_warning(Mix_GetError());
    SDL_LockAudio();
    cleanup_channel(chan);
    SDL_UnlockAudio();
    return 0;
}

glui32 glk_schannel_play_ext_impl(schanid_t chan, glui32 snd, glui32 repeats, glui32 notify, std::function<glui32(glui32, std::vector<unsigned char> &)> load_resource)
{
    glui32 type;
    glui32 result = 0;
    bool paused = false;

    if (!chan) {
        gli_strict_warning("schannel_play_ext: invalid id.");
        return 0;
    }

    // store paused state of channel
    paused = chan->paused;

    // stop previous noise
    glk_schannel_stop(chan);

    if (repeats == 0) {
        return 1;
    }

    // load sound resource into memory
    try {
        type = load_resource(snd, chan->sdl_memory);
    } catch (const Bleeps::Empty &) {
        return 1;
    }

    chan->sdl_rwops = SDL_RWFromConstMem(chan->sdl_memory.data(), chan->sdl_memory.size());
    chan->notify = notify;
    chan->resid = snd;
    chan->loop = repeats;

    switch (type) {
    case giblorb_ID_FORM:
    case giblorb_ID_AIFF:
    case giblorb_ID_WAVE:
    case giblorb_ID_OGG:
    case giblorb_ID_MP3:
        result = play_sound(chan);
        break;

    case giblorb_ID_MOD:
    case giblorb_ID_MIDI:
        result = play_mod(chan, chan->sdl_memory.size());
        break;

    default:
        gli_strict_warning("schannel_play_ext: unknown resource type.");
    }

    // if channel was paused it should be paused again
    if (result && paused) {
        glk_schannel_pause(chan);
    }

    return result;
}

glui32 glk_schannel_play_ext(schanid_t chan, glui32 snd, glui32 repeats, glui32 notify)
{
    return glk_schannel_play_ext_impl(chan, snd, repeats, notify, load_sound_resource);
}

void glk_schannel_pause(schanid_t chan)
{
    if (!chan) {
        gli_strict_warning("schannel_pause: invalid id.");
        return;
    }

    switch (chan->status) {
    case CHANNEL_SOUND:
        Mix_Pause(chan->sdl_channel);
        break;
    case CHANNEL_MUSIC:
        Mix_PauseMusic();
        break;
    }

    chan->paused = true;
}

void glk_schannel_unpause(schanid_t chan)
{
    if (!chan) {
        gli_strict_warning("schannel_unpause: invalid id.");
        return;
    }
    switch (chan->status) {
    case CHANNEL_SOUND:
        Mix_Resume(chan->sdl_channel);
        break;
    case CHANNEL_MUSIC:
        Mix_ResumeMusic();
        break;
    }

    chan->paused = false;
}

void glk_schannel_stop(schanid_t chan)
{
    if (!chan) {
        gli_strict_warning("schannel_stop: invalid id.");
        return;
    }
    SDL_LockAudio();
    chan->paused = false;
    glk_schannel_unpause(chan);
    SDL_UnlockAudio();
    switch (chan->status) {
    case CHANNEL_SOUND:
        chan->notify = 0;
        Mix_HaltChannel(chan->sdl_channel);
        break;
    case CHANNEL_MUSIC:
        if (music_channel == chan) {
            Mix_HookMusicFinished(nullptr);
        }
        Mix_HaltMusic();
        break;
    }
    SDL_LockAudio();
    cleanup_channel(chan);
    SDL_UnlockAudio();
}

void garglk_zbleep(glui32 number)
{
    if (gli_bleep_channel == nullptr) {
        gli_bleep_channel = glk_schannel_create(0);
        if (gli_bleep_channel != nullptr) {
            glk_schannel_set_volume(gli_bleep_channel, 0x8000);
        }
    }

    if (gli_bleep_channel != nullptr) {
        glk_schannel_play_ext_impl(gli_bleep_channel, number, 1, 0, load_bleep_resource);
    }
}
