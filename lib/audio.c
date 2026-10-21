#include "native.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

typedef struct {
    bool valid;
    uint32_t rate, count, offset;
    Buffer bytes;
} Audio;
typedef struct { char *name, *path; bool consumed; } External;
typedef struct { int id; char *path; } AudioGroup;
typedef struct { char *path; Audio audio; } Music;
typedef struct {
    External *external; size_t external_count;
    AudioGroup *groups; size_t group_count;
    Music *music; size_t music_count;
} Files;

static void count16(size_t n, const char *what) {
    if (n > UINT16_MAX) fail("Too many %s: %zu (maximum 65535)", what, n);
}

static bool extension(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && strcasecmp(name + n - e, ext) == 0;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int compare_music(const void *a, const void *b) {
    return strcmp(((const Music *)a)->path, ((const Music *)b)->path);
}

static int group_id(const char *name) {
    if (strlen(name) < 15 || strncasecmp(name, "audiogroup", 10)) return -1;
    const char *p = name + 10;
    if (*p < '0' || *p > '9') return -1;
    uint32_t id = 0;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (id > ((uint32_t)INT_MAX - digit) / 10)
            fail("Audiogroup number exceeds signed 32-bit range: %s", name);
        id = id * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    return strcasecmp(p, ".dat") == 0 ? (int)id : -1;
}

static void scan_directory(Files *files, const char *root, const char *relative,
                           unsigned depth) {
    if (depth > 256) fail("Audio directory nesting exceeds 256: %s", relative);
    char *directory = *relative ? join_path(root, relative) : copy_string(root);
    DIR *dir = opendir(directory);
    if (!dir) fail("Cannot open audio directory %s: %s", directory, strerror(errno));
    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (count == SIZE_MAX / sizeof(*names)) fail("Too many audio directory entries");
        names = resize_array(names, count + 1, sizeof(*names));
        names[count++] = copy_string(entry->d_name);
        errno = 0;
    }
    if (errno) fail("Cannot read audio directory %s: %s", directory, strerror(errno));
    if (closedir(dir)) fail("Cannot close audio directory %s: %s", directory, strerror(errno));
    if (count > 1) qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; ++i) {
        char *rel = *relative ? join_path(relative, names[i]) : copy_string(names[i]);
        char *path = join_path(root, rel);
        struct stat st;
        if (lstat(path, &st)) fail("Cannot inspect audio path %s: %s", path, strerror(errno));
        bool symlink = S_ISLNK(st.st_mode);
        if (symlink && stat(path, &st))
            fail("Cannot follow audio file %s: %s", path, strerror(errno));
        if (S_ISDIR(st.st_mode)) {
            if (!symlink) scan_directory(files, root, rel, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            int id = *relative ? -1 : group_id(names[i]);
            if (id > 0) {
                for (size_t j = 0; j < files->group_count; ++j)
                    if (files->groups[j].id == id)
                        fail("Ambiguous duplicate audiogroup %d: %s", id, path);
                files->groups = resize_array(files->groups, files->group_count + 1,
                                             sizeof(*files->groups));
                files->groups[files->group_count++] =
                    (AudioGroup){id, copy_string(path)};
            } else if (!*relative && (extension(names[i], ".wav") || extension(names[i], ".ogg"))) {
                files->external = resize_array(files->external, files->external_count + 1,
                                               sizeof(*files->external));
                files->external[files->external_count++] =
                    (External){copy_string(names[i]), copy_string(path), false};
            } else if (*relative && extension(names[i], ".ogg")) {
                if (strlen(rel) > UINT8_MAX)
                    fail("Music path exceeds 255 UTF-8 bytes: %s", rel);
                count16(files->music_count + 1, "music entries");
                files->music = resize_array(files->music, files->music_count + 1,
                                            sizeof(*files->music));
                files->music[files->music_count++] = (Music){copy_string(rel), {0}};
            }
        }
        free(path);
        free(rel);
        free(names[i]);
    }
    free(names);
    free(directory);
}

static int clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

typedef struct {
    Audio audio;
    unsigned channels;
    double ratio;
    size_t frames, encoded;
    int predictor, index;
    int16_t *previous;
} PcmEncoder;

static size_t pcm_chunk_frames(unsigned channels) {
    return channels > 4096 ? 1 : 4096 / channels;
}

static PcmEncoder new_pcm_encoder(unsigned rate, unsigned channels) {
    if (!rate || !channels || channels > UINT16_MAX)
        fail("Invalid audio sample rate or channel count: %u Hz, %u channels", rate, channels);
    return (PcmEncoder){
        .audio={.valid=true,.rate=rate>22050 ? 22050:rate},
        .channels=channels,
        .ratio=rate>22050 ? (double)rate/22050.0:1.0,
        .previous=allocate(channels,sizeof(int16_t))
    };
}

static void encode_adpcm(PcmEncoder *encoder, int sample) {
    static const int steps[] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
        34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
        157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
        724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
        3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
        15289,16818,18500,20350,22385,24623,27086,29794,32767
    };
    static const int indices[] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
    int step = steps[encoder->index], diff = sample - encoder->predictor, nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    if (diff >= step) { nibble |= 4; diff -= step; }
    if (diff >= step / 2) { nibble |= 2; diff -= step / 2; }
    if (diff >= step / 4) nibble |= 1;
    int delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;
    encoder->predictor = clamp_int(encoder->predictor + ((nibble & 8) ? -delta : delta), -32768, 32767);
    encoder->index = clamp_int(encoder->index + indices[nibble], 0, 88);
    Buffer *out=&encoder->audio.bytes;
    if (encoder->encoded & 1) out->data[out->size-1] |= (uint8_t)(nibble << 4);
    else put8(out,(unsigned)nibble);
    ++encoder->encoded;
}

static void encode_pcm(PcmEncoder *encoder, const int16_t *pcm, size_t frames, bool final) {
    if (frames > UINT32_MAX - encoder->frames) fail("Audio sample count exceeds 32-bit range");
    size_t base=encoder->frames,end=base+frames;
    size_t output_frames=(size_t)(end/encoder->ratio);
    while (!final || encoder->encoded<output_frames) {
        double position = encoder->encoded * encoder->ratio;
        size_t source = encoder->ratio>1.0 ? (size_t)position : encoder->encoded;
        if (source>=end || (!final && encoder->ratio>1.0 && source+1>=end)) break;
        if (source<base && (!base || source!=base-1)) fail("PCM resampler lost source frames");
        float fraction = (float)(position - source);
        int sum = 0;
        for (unsigned ch = 0; ch < encoder->channels; ++ch) {
            int first = source<base ? encoder->previous[ch] : pcm[(source-base)*encoder->channels+ch];
            int value = first;
            if (encoder->ratio>1.0) {
                int second = source+1<end ? pcm[(source+1-base)*encoder->channels+ch] : first;
                /* Kotlin performs this interpolation in Float, then truncates. */
                float difference = (float)(second - first) * fraction;
                value = clamp_int((int)((float)first + difference), -32768, 32767);
            }
            sum += value;
        }
        encode_adpcm(encoder,sum/(int)encoder->channels);
    }
    if (frames) memcpy(encoder->previous,pcm+(frames-1)*encoder->channels,
                       encoder->channels*sizeof(*pcm));
    encoder->frames=end;
}

static Audio finish_pcm(PcmEncoder *encoder) {
    encode_pcm(encoder,NULL,0,true);
    size_t count=(size_t)(encoder->frames/encoder->ratio);
    if (encoder->encoded<count) fail("PCM resampler produced too few samples");
    /* Until EOF, one speculative sample may precede the final floor(frames / ratio) limit. */
    encoder->audio.count=(uint32_t)count;
    encoder->audio.bytes.size=count/2+count%2;
    if (count&1) encoder->audio.bytes.data[encoder->audio.bytes.size-1]&=15;
    free(encoder->previous);
    return encoder->audio;
}

static Audio decode_wav(Slice data, const char *label) {
    if (data.size < 12)
        fail("Corrupt RIFF/WAVE header: %s", label);
    if (memcmp(data.data + 8, "WAVE", 4)) {
        fprintf(stderr, "Unsupported non-WAVE RIFF audio: %s (unmapped)\n", label);
        return (Audio){0};
    }
    size_t end = (size_t)u32(data, 4) + 8;
    if (end < 12 || end > data.size) fail("Truncated RIFF/WAVE: %s", label);
    unsigned rate = 0, channels = 0, bits = 0, format = 0;
    bool found_fmt = false, found_data = false;
    Slice pcm = {0};
    for (size_t pos = 12; pos < end;) {
        if (end - pos < 8) fail("Truncated WAV chunk header: %s", label);
        size_t size = u32(data, pos + 4), start = pos + 8;
        if (size > end - start) fail("Truncated WAV chunk: %s", label);
        if (!memcmp(data.data + pos, "fmt ", 4)) {
            if (size < 16) fail("Truncated WAV fmt chunk: %s", label);
            format = u16(data, start);
            channels = u16(data, start + 2);
            rate = u32(data, start + 4);
            bits = u16(data, start + 14);
            found_fmt = true;
        } else if (!memcmp(data.data + pos, "data", 4)) {
            pcm = section(data, start, size);
            found_data = true;
        }
        pos = start + size;
        if (pos & 1) {
            if (pos == end) fail("Missing WAV chunk padding: %s", label);
            ++pos;
        }
    }
    if (!found_fmt || !found_data) fail("Missing WAV fmt/data chunk: %s", label);
    if (format != 1 || (bits != 8 && bits != 16)) {
        fprintf(stderr, "Unsupported WAV format %u/%u-bit: %s (unmapped)\n", format, bits, label);
        return (Audio){0};
    }
    if (!rate || rate > INT_MAX || !channels) fail("Invalid WAV format fields: %s", label);
    size_t frames=pcm.size/(bits/8)/channels;
    size_t chunk_frames=pcm_chunk_frames(channels);
    int16_t *samples=allocate(chunk_frames*channels,sizeof(*samples));
    PcmEncoder encoder=new_pcm_encoder(rate,channels);
    for (size_t at=0;at<frames;) {
        size_t n=frames-at<chunk_frames ? frames-at:chunk_frames;
        for (size_t i=0;i<n*channels;++i) {
            size_t source=at*channels+i;
            if (bits == 16) {
                unsigned value = u16(pcm, source*2);
                samples[i] = (int16_t)(value >= 32768 ? (int)value - 65536 : (int)value);
            } else {
                /* Preserve Kotlin's wrapping 257 multiplier, including unsigned zero. */
                int value = ((int)pcm.data[source] - 128) * 257;
                if (value < -32768) value += 65536;
                samples[i] = (int16_t)value;
            }
        }
        encode_pcm(&encoder,samples,n,false);
        at+=n;
    }
    free(samples);
    return finish_pcm(&encoder);
}

static bool validate_ogg(Slice data, const char *label) {
    bool vorbis = false, ended = false;
    for (size_t pos = 0; pos < data.size;) {
        if (data.size - pos < 27) fail("Truncated OGG page header: %s", label);
        Slice header = section(data, pos, 27);
        if (memcmp(header.data, "OggS", 4) || header.data[4] != 0)
            fail("Corrupt OGG page header: %s", label);
        size_t segments = header.data[26];
        if (segments > data.size - pos - 27) fail("Truncated OGG lacing table: %s", label);
        Slice lacing = section(data, pos + 27, segments);
        size_t payload_size = 0;
        for (size_t i = 0; i < segments; ++i) payload_size += lacing.data[i];
        size_t payload_at = pos + 27 + segments;
        if (payload_size > data.size - payload_at) fail("Truncated OGG page payload: %s", label);
        Slice payload = section(data, payload_at, payload_size);
        if (!pos && payload.size >= 7 && payload.data[0] == 1 &&
            !memcmp(payload.data + 1, "vorbis", 6)) vorbis = true;
        ended = (header.data[5] & 4) != 0;
        pos = payload_at + payload_size;
    }
    if (!ended) fail("Missing final OGG end-of-stream page: %s", label);
    return vorbis;
}

static Audio decode_ogg(Slice data, const char *label) {
    bool identified_vorbis = validate_ogg(data, label);
    if (!identified_vorbis) {
        fprintf(stderr, "Unsupported OGG codec: %s (unmapped)\n", label);
        return (Audio){0};
    }
    if (data.size > INT_MAX) fail("Vorbis input exceeds decoder limit: %s", label);
    int error=0;
    stb_vorbis *vf=stb_vorbis_open_memory(data.data,(int)data.size,&error,NULL);
    if (!vf) fail("Corrupt OGG/Vorbis %s: decoder error %d",label,error);
    stb_vorbis_info info=stb_vorbis_get_info(vf);
    unsigned total=stb_vorbis_stream_length_in_samples(vf);
    unsigned rate=info.sample_rate,channels=(unsigned)info.channels;
    if (!rate || rate>INT_MAX || !channels || channels>UINT16_MAX)
        fail("Invalid Vorbis stream metadata: %s",label);
    if (!total) {
        stb_vorbis_close(vf);
        fprintf(stderr, "Empty Vorbis stream: %s (unmapped)\n", label);
        return (Audio){0};
    }
    if (total>INT_MAX/channels) fail("Vorbis sample allocation exceeds decoder limit: %s",label);
    size_t chunk_frames=pcm_chunk_frames(channels);
    size_t chunk_capacity=chunk_frames*channels;
    float *samples=allocate(chunk_capacity,sizeof *samples);
    int16_t *pcm=allocate(chunk_capacity,sizeof *pcm);
    PcmEncoder encoder=new_pcm_encoder(rate,channels);
    size_t count=0;
    while (count<total) {
        size_t remaining=total-count;
        size_t requested=remaining<chunk_frames ? remaining:chunk_frames;
        int frames=stb_vorbis_get_samples_float_interleaved(vf,(int)channels,samples,
                   (int)(requested*channels));
        if (frames<=0) break;
        size_t n=(size_t)frames*channels;
        for (size_t i=0;i<n;++i) {
            float value=samples[i];
            if (!isfinite(value)) fail("Non-finite Vorbis PCM sample: %s",label);
            if (value < -1.0f) value=-1.0f;
            if (value > 1.0f) value=1.0f;
            pcm[i]=(int16_t)(int)(value*32767.0f);
        }
        encode_pcm(&encoder,pcm,(size_t)frames,false);
        count+=(size_t)frames;
    }
    error=stb_vorbis_get_error(vf);
    stb_vorbis_close(vf);
    if (error || !count) fail("Corrupt Vorbis sample data: %s (decoder error %d)",label,error);
    free(samples);
    free(pcm);
    return finish_pcm(&encoder);
}

static Audio decode_audio(Slice data, const char *label) {
    if (!data.data) {
        fprintf(stderr, "Missing audio: %s (unmapped)\n", label);
        return (Audio){0};
    }
    if (data.size >= 4 && !memcmp(data.data, "RIFF", 4)) return decode_wav(data, label);
    if (data.size >= 4 && !memcmp(data.data, "OggS", 4)) return decode_ogg(data, label);
    fprintf(stderr, "Unsupported audio: %s (unmapped)\n", label);
    return (Audio){0};
}

static size_t append_audio(Audio **items, size_t *count, Audio audio) {
    count16(*count + 1, "AUDO entries");
    *items = resize_array(*items, *count + 1, sizeof(**items));
    (*items)[*count] = audio;
    return (*count)++;
}

static void append_samples(FILE *sounds, size_t *size, Audio *audio) {
    if (!audio->valid) return;
    if (*size > UINT32_MAX || audio->bytes.size > UINT32_MAX - *size)
        fail("SOUNDS.BIN exceeds 32-bit offsets");
    audio->offset = (uint32_t)*size;
    if (audio->bytes.size && fwrite(audio->bytes.data,1,audio->bytes.size,sounds)!=audio->bytes.size)
        fail("Cannot write SOUNDS.BIN: %s",strerror(errno));
    *size += audio->bytes.size;
}

static void free_audio_bytes(Audio *audio) {
    if (!audio || !audio->bytes.data) return;
    free(audio->bytes.data);
    audio->bytes.data = NULL;
}

static int32_t kotlin_int(float value) {
    if (isnan(value)) return 0;
    if ((double)value >= INT32_MAX) return INT32_MAX;
    if ((double)value <= INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

void process_audio(const DataWin *dw, const char *input_directory,
                   Buffer *sound_bank, FILE *sounds) {
    if (bspp_verbose) fprintf(stderr, "Audio: processing from directory %s\n", input_directory);
    if (sound_bank->size) fail("Sound bank buffer must initially be empty");
    size_t sounds_size=0;
    count16(dw->audio_count, "embedded AUDO entries");
    count16(dw->sound_count, "SOND entries");
    Files files = {0};
    scan_directory(&files, input_directory, "", 0);
    if (files.music_count > 1)
        qsort(files.music, files.music_count, sizeof(*files.music), compare_music);
    size_t audio_count = dw->audio_count;
    Audio *audio = allocate(audio_count, sizeof(*audio));
    uint16_t *mapping = allocate(dw->sound_count, sizeof(*mapping));
    for (size_t i = 0; i < dw->sound_count; ++i) mapping[i] = UINT16_MAX;
    for (size_t i = 0; i < audio_count; ++i) {
        char *label = format_string("embedded AUDO #%zu", i);
        audio[i] = decode_audio(dw->audio[i], label);
        free(label);
        if (audio[i].valid) {
            append_samples(sounds, &sounds_size, &audio[i]);
            free_audio_bytes(&audio[i]);
        }
    }
    /* Group decodes append in SOND order, before any external file decodes. */
    DataWin loaded_group={0};
    size_t loaded_index=SIZE_MAX;
    for (size_t i = 0; i < dw->sound_count; ++i) {
        const Sound *sound = &dw->sounds[i];
        if (!sound->group) continue;
        size_t group_index=SIZE_MAX;
        for (size_t g = 0; g < files.group_count; ++g)
            if (files.groups[g].id == sound->group) group_index=g;
        const DataWin *group=NULL;
        if (group_index!=SIZE_MAX) {
            if (group_index!=loaded_index) {
                free_datawin(&loaded_group);
                const char *path=files.groups[group_index].path;
                if (bspp_verbose) fprintf(stderr,"Parsing %s\n",path);
                loaded_group=load_datawin(path,true,false);
                loaded_index=group_index;
            }
            group=&loaded_group;
        }
        if (!group || sound->audio < 0 || (size_t)sound->audio >= group->audio_count) {
            fprintf(stderr, "Missing audiogroup audio %d/%d for SOND #%zu (unmapped group)\n",
                    sound->group, sound->audio, i);
            continue;
        }
        char *label = format_string("audiogroup%d/%d (SOND #%zu)", sound->group, sound->audio, i);
        Audio decoded = decode_audio(group->audio[sound->audio], label);
        free(label);
        if (decoded.valid) {
            append_samples(sounds, &sounds_size, &decoded);
            mapping[i] = (uint16_t)append_audio(&audio, &audio_count, decoded);
            free_audio_bytes(&audio[mapping[i]]);
        }
    }
    free_datawin(&loaded_group);
    for (size_t i = 0; i < dw->sound_count; ++i) {
        const Sound *sound = &dw->sounds[i];
        if ((sound->flags & 1) || mapping[i] != UINT16_MAX) continue;
        External *external = NULL;
        if (sound->file) {
            for (unsigned candidate = 0; candidate < 3 && !external; ++candidate) {
                char *name = format_string("%s%s", sound->file,
                                           candidate == 0 ? "" : candidate == 1 ? ".ogg" : ".wav");
                for (size_t j = 0; j < files.external_count; ++j)
                    if (!strcmp(name, files.external[j].name)) { external = &files.external[j]; break; }
                free(name);
            }
        }
        if (!external) {
            fprintf(stderr, "Missing external audio for SOND #%zu (%s); original AUDO fallback retained\n",
                    i, sound->file ? sound->file : "(no filename)");
            continue;
        }
        Buffer data=external->consumed ? (Buffer){0}:read_file(external->path);
        Audio decoded = decode_audio((Slice){data.data,data.size}, external->name);
        if (decoded.valid) {
            append_samples(sounds, &sounds_size, &decoded);
            mapping[i] = (uint16_t)append_audio(&audio, &audio_count, decoded);
            free_audio_bytes(&audio[mapping[i]]);
        }
        free(data.data);
        external->consumed=true;
    }
    size_t music_count = 0;
    for (size_t i = 0; i < files.music_count; ++i) {
        char *path = join_path(input_directory, files.music[i].path);
        Buffer data = read_file(path);
        files.music[i].audio = decode_audio((Slice){data.data, data.size}, files.music[i].path);
        free(data.data);
        free(path);
        if (files.music[i].audio.valid) {
            append_samples(sounds, &sounds_size, &files.music[i].audio);
            free_audio_bytes(&files.music[i].audio);
            ++music_count;
        }
    }
    put8(sound_bank, 0);
    put16(sound_bank, dw->sound_count, false);
    put16(sound_bank, audio_count, false);
    put16(sound_bank, music_count, false);
    for (size_t i = 0; i < dw->sound_count; ++i) {
        const Sound *sound = &dw->sounds[i];
        uint16_t index = mapping[i];
        if (index == UINT16_MAX && sound->audio >= 0 && (size_t)sound->audio < audio_count)
            index = (uint16_t)sound->audio;
        put16(sound_bank, index, false);
        put32(sound_bank, sound->flags, false);
        put16(sound_bank, (uint16_t)kotlin_int(sound->volume * 256.0f), false);
        put16(sound_bank, (uint16_t)kotlin_int(sound->pitch * 256.0f), false);
        put16(sound_bank, 0, false);
    }
    for (size_t i = 0; i < audio_count; ++i) {
        Audio *a = &audio[i];
        put32(sound_bank, a->valid ? a->offset : 0, false);
        put32(sound_bank, a->bytes.size, false);
        put16(sound_bank, a->rate, false);
        put8(sound_bank, a->valid ? 1 : 0);
        put8(sound_bank, a->valid ? 4 : 0);
        put8(sound_bank, a->valid ? 1 : 0);
        put8(sound_bank, 0);
        put16(sound_bank, 0, false);
        put32(sound_bank, a->count, false);
        free(a->bytes.data);
    }
    for (size_t i = 0; i < files.music_count; ++i) {
        if (!files.music[i].audio.valid) continue;
        size_t length = strlen(files.music[i].path);
        put8(sound_bank, (unsigned)length);
        append(sound_bank, files.music[i].path, length);
    }
    for (size_t i = 0; i < files.music_count; ++i) {
        Audio *a = &files.music[i].audio;
        if (a->valid) {
            put32(sound_bank, a->offset, false);
            put32(sound_bank, a->bytes.size, false);
            put16(sound_bank, a->rate, false);
            put8(sound_bank, 1);
            put8(sound_bank, 1);
            put32(sound_bank, a->count, false);
        }
        free(a->bytes.data);
        free(files.music[i].path);
    }
    if (bspp_verbose) fprintf(stderr, "Audio: %zu AUDO entries, %zu music tracks, %zu sound bytes\n",
                               audio_count, music_count, sounds_size);
    for (size_t i = 0; i < files.external_count; ++i) {
        free(files.external[i].name);
        free(files.external[i].path);
    }
    for (size_t i = 0; i < files.group_count; ++i) free(files.groups[i].path);
    free(files.external);
    free(files.groups);
    free(files.music);
    free(mapping);
    free(audio);
}
