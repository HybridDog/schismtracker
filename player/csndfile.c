/*
 * Schism Tracker - a cross-platform Impulse Tracker clone
 * copyright (c) 2003-2005 Storlek <storlek@rigelseven.com>
 * copyright (c) 2005-2008 Mrs. Brisby <mrs.brisby@nimh.org>
 * copyright (c) 2009 Storlek & Mrs. Brisby
 * copyright (c) 2010 Storlek
 * URL: http://schismtracker.org/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define NEED_BYTESWAP

#include <math.h>
#include <stdint.h>
#include <assert.h>

#include "sndfile.h"
#include "log.h"
#include "util.h"
#include "fmt.h" // for it_decompress8 / it_decompress16


static void _csf_reset(song_t *csf)
{
        unsigned int i;

        csf->flags = 0;
        csf->pan_separation = 128;
        csf->num_voices = 0;
        csf->freq_factor = csf->tempo_factor = 128;
        csf->initial_global_volume = 128;
        csf->current_global_volume = 128;
        csf->initial_speed = 6;
        csf->initial_tempo = 125;
        csf->process_row = 0;
        csf->row = 0;
        csf->current_pattern = 0;
        csf->current_order = 0;
        csf->process_order = 0;
        csf->mixing_volume = 0x30;
        memset(csf->message, 0, sizeof(csf->message));

        csf->row_highlight_major = 16;
        csf->row_highlight_minor = 4;

        memset(csf->voices, 0, sizeof(csf->voices));
        memset(csf->voice_mix, 0, sizeof(csf->voice_mix));
        memset(csf->samples, 0, sizeof(csf->samples));
        memset(csf->instruments, 0, sizeof(csf->instruments));
        memset(csf->orderlist, 0xFF, sizeof(csf->orderlist));
        memset(csf->patterns, 0, sizeof(csf->patterns));

        csf_reset_midi_cfg(csf);

        for (i = 0; i < MAX_PATTERNS; i++) {
                csf->pattern_size[i] = 64;
                csf->pattern_alloc_size[i] = 64;
        }
        for (i = 0; i < MAX_SAMPLES; i++) {
                csf->samples[i].c5speed = 8363;
                csf->samples[i].volume = 64 * 4;
                csf->samples[i].global_volume = 64;
        }
        for (i = 0; i < MAX_CHANNELS; i++) {
                csf->channels[i].panning = 128;
                csf->channels[i].volume = 64;
                csf->channels[i].flags = 0;
        }
}

//////////////////////////////////////////////////////////
// song_t

song_t *csf_allocate(void)
{
        song_t *csf = calloc(1, sizeof(song_t));
        _csf_reset(csf);
        return csf;
}

void csf_free(song_t *csf)
{
        if (csf) {
                csf_destroy(csf);
                free(csf);
        }
}


static void _init_envelope(song_envelope_t *env, int n)
{
        env->nodes = 2;
        env->ticks[0] = 0;
        env->ticks[1] = 100;
        env->values[0] = n;
        env->values[1] = n;
}

void csf_init_instrument(song_instrument_t *ins, int samp)
{
        int n;
        _init_envelope(&ins->vol_env, 64);
        _init_envelope(&ins->pan_env, 32);
        _init_envelope(&ins->pitch_env, 32);
        ins->global_volume = 128;
        ins->panning = 128;
        ins->midi_bank = -1;
        ins->midi_program = -1;
        ins->pitch_pan_center = 60; // why does pitch/pan not use the same note values as everywhere else?!
        for (n = 0; n < 128; n++) {
                ins->sample_map[n] = samp;
                ins->note_map[n] = n + 1;
        }
}

song_instrument_t *csf_allocate_instrument(void)
{
        song_instrument_t *ins = calloc(1, sizeof(song_instrument_t));
        csf_init_instrument(ins, 0);
        return ins;
}

void csf_free_instrument(song_instrument_t *i)
{
        free(i);
}


void csf_destroy(song_t *csf)
{
        int i;

        for (i = 0; i < MAX_PATTERNS; i++) {
                if (csf->patterns[i]) {
                        csf_free_pattern(csf->patterns[i]);
                        csf->patterns[i] = NULL;
                }
        }
        for (i = 1; i < MAX_SAMPLES; i++) {
                song_sample_t *pins = &csf->samples[i];
                if (pins->data) {
                        csf_free_sample(pins->data);
                        pins->data = NULL;
                }
        }
        for (i = 0; i < MAX_INSTRUMENTS; i++) {
                if (csf->instruments[i]) {
                        csf_free_instrument(csf->instruments[i]);
                        csf->instruments[i] = NULL;
                }
        }

        _csf_reset(csf);
}

song_note_t *csf_allocate_pattern(uint32_t rows)
{
        return calloc(rows * MAX_CHANNELS, sizeof(song_note_t));
}

void csf_free_pattern(void *pat)
{
        free(pat);
}

/* Note: this function will appear in valgrind to be a sieve for memory leaks.
It isn't; it's just being confused by the adjusted pointer being stored. */
signed char *csf_allocate_sample(uint32_t nbytes)
{
        signed char *p = calloc(1, (nbytes + 39) & ~7); // magic
        if (p)
                p += 16;
        return p;
}

void csf_free_sample(void *p)
{
        if (p)
                free(p - 16);
}


//////////////////////////////////////////////////////////////////////////
// Misc functions

midi_config_t default_midi_config;


void csf_reset_midi_cfg(song_t *csf)
{
        memcpy(&csf->midi_config, &default_midi_config, sizeof(default_midi_config));
}


int csf_set_wave_config(song_t *csf, uint32_t rate,uint32_t bits,uint32_t channels)
{
        int reset = ((mix_frequency != rate) || (mix_bits_per_sample != bits) || (mix_channels != channels));
        mix_channels = channels;
        mix_frequency = rate;
        mix_bits_per_sample = bits;
        csf_init_player(csf, reset);
//printf("Rate=%u Bits=%u Channels=%u\n",mix_frequency,mix_bits_per_sample,mix_channels);
        return 1;
}


int csf_set_resampling_mode(UNUSED song_t *csf, uint32_t mode)
{
        uint32_t d = mix_flags & ~(SNDMIX_NORESAMPLING|SNDMIX_HQRESAMPLER|SNDMIX_ULTRAHQSRCMODE);
        switch(mode) {
                case SRCMODE_NEAREST:   d |= SNDMIX_NORESAMPLING; break;
                case SRCMODE_LINEAR:    break;
                case SRCMODE_SPLINE:    d |= SNDMIX_HQRESAMPLER; break;
                case SRCMODE_POLYPHASE: d |= (SNDMIX_HQRESAMPLER|SNDMIX_ULTRAHQSRCMODE); break;
                default:                return 0;
        }
        mix_flags = d;
        return 1;
}


// IT-compatible...
uint32_t csf_get_num_orders(song_t *csf)
{
        uint32_t i = 0;
        while (i < MAX_ORDERS && csf->orderlist[i] < 0xFF)
                i++;
        return i ? i - 1 : 0;
}



// This used to use some retarded positioning based on the total number of rows elapsed, which is useless.
// However, the only code calling this function is in this file, to set it to the start, so I'm optimizing
// out the row count.
static void set_current_pos_0(song_t *csf)
{
        song_voice_t *v = csf->voices;
        for (uint32_t i = 0; i < MAX_VOICES; i++, v++) {
                memset(v, 0, sizeof(*v));
                v->cutoff = 0x7F;
                v->volume = 256;
                if (i < MAX_CHANNELS) {
                        v->panning = csf->channels[i].panning;
                        v->global_volume = csf->channels[i].volume;
                        v->flags = csf->channels[i].flags;
                } else {
                        v->panning = 128;
                        v->global_volume = 64;
                }
        }
        csf->current_global_volume = csf->initial_global_volume;
        csf->current_speed = csf->initial_speed;
        csf->current_tempo = csf->initial_tempo;
}


void csf_set_current_order(song_t *csf, uint32_t position)
{
        for (uint32_t j = 0; j < MAX_VOICES; j++) {
                song_voice_t *v = csf->voices + j;

                v->period = 0;
                v->note = v->new_note = v->new_instrument = 0;
                v->portamento_target = 0;
                v->n_command = 0;
                v->cd_patloop = 0;
                v->patloop_row = 0;
                v->cd_tremor = 0;
                // modplug sets vib pos to 16 in old effects mode for some reason *shrug*
                v->vibrato_position = (csf->flags & SONG_ITOLDEFFECTS) ? 0 : 0x10;
                v->tremolo_position = 0;
        }
        if (position > MAX_ORDERS)
                position = 0;
        if (!position)
                set_current_pos_0(csf);

        csf->process_order = position - 1;
        csf->process_row = PROCESS_NEXT_ORDER;
        csf->row = 0;
        csf->break_row = 0; /* set this to whatever row to jump to */
        csf->tick_count = 1;
        csf->row_count = 0;
        csf->buffer_count = 0;

        csf->flags &= ~(SONG_PATTERNLOOP|SONG_ENDREACHED);
}

void csf_reset_playmarks(song_t *csf)
{
        int n;

        for (n = 1; n < MAX_SAMPLES; n++) {
                csf->samples[n].played = 0;
        }
        for (n = 1; n < MAX_INSTRUMENTS; n++) {
                if (csf->instruments[n])
                        csf->instruments[n]->played = 0;
        }
}


void csf_loop_pattern(song_t *csf, int pat, int row)
{
        if (pat < 0 || pat >= MAX_PATTERNS || !csf->patterns[pat]) {
                csf->flags &= ~SONG_PATTERNLOOP;
        } else {
                if (row < 0 || row >= csf->pattern_size[pat])
                        row = 0;
                csf->process_order = 0; /* whatever */
                csf->break_row = row;
                csf->tick_count = 1;
                csf->row_count = 0;
                csf->current_pattern = pat;
                csf->buffer_count = 0;
                csf->flags |= SONG_PATTERNLOOP;
        }
}

/* --------------------------------------------------------------------------------------------------------- */

#define SF_FAIL(name, n) \
        ({ log_appendf(4, "%s: internal error: unsupported %s %d", __FUNCTION__, name, n); return 0; })

uint32_t csf_write_sample(disko_t *fp, song_sample_t *sample, uint32_t flags)
{
        uint32_t pos, len = sample->length;
        int stride;     // how much to add to the left/right pointer per sample written
        int rightofs;   // where the right channel is in relation to the left
        int byteswap;   // should the sample data be byte-swapped?
        int add;        // how much to add to the sample data (for converting to unsigned)

        // validate the write flags, and set up the save params
        switch (flags & SF_CHN_MASK) {
        case SF_SI:
                if (!(sample->flags & CHN_STEREO))
                        SF_FAIL("channel mask", flags & SF_CHN_MASK);
                stride = 1;
                rightofs = sample->length;
                len *= 2;
                break;
        case SF_SS:
                if (!(sample->flags & CHN_STEREO))
                        SF_FAIL("channel mask", flags & SF_CHN_MASK);
                stride = 2;
                rightofs = 1;
                len *= 2;
                break;
        case SF_M:
                /* Mono is actually processed the same as interleaved stereo, except without doubling
                the data length. An extra "left" sample is written if the sample size is odd. */
                stride = 2;
                rightofs = 1;
                break;
        default:
                SF_FAIL("channel mask", flags & SF_CHN_MASK);
        }

        // TODO allow converting bit width, this will be useful
        if ((flags & SF_BIT_MASK) != ((sample->flags & CHN_16BIT) ? SF_16 : SF_8))
                SF_FAIL("bit width", flags & SF_BIT_MASK);

        switch (flags & SF_END_MASK) {
#if WORDS_BIGENDIAN
        case SF_LE:
                byteswap = 1;
                break;
        case SF_BE:
                byteswap = 0;
                break;
#else
        case SF_LE:
                byteswap = 0;
                break;
        case SF_BE:
                byteswap = 1;
                break;
#endif
        default:
                SF_FAIL("endianness", flags & SF_END_MASK);
        }

        switch (flags & SF_ENC_MASK) {
        case SF_PCMU:
                add = ((flags & SF_BIT_MASK) == SF_16) ? 32768 : 128;
                break;
        case SF_PCMS:
                add = 0;
                break;
        default:
                SF_FAIL("encoding", flags & SF_ENC_MASK);
        }

        if ((flags & ~(SF_BIT_MASK | SF_CHN_MASK | SF_END_MASK | SF_ENC_MASK)) != 0) {
                SF_FAIL("extra flag", flags & ~(SF_BIT_MASK | SF_CHN_MASK | SF_END_MASK | SF_ENC_MASK));
        }

        if (!sample || sample->length < 1 || sample->length > MAX_SAMPLE_LENGTH || !sample->data)
                return 0;

        if ((flags & SF_BIT_MASK) == SF_16) {
                // 16-bit data.

                // 'left' and 'right' samples are written in an alternating manner
                const int16_t *left, *right;
                int16_t v;

                left = (const int16_t *) sample->data;
                right = left + rightofs;

                for (pos = 0; pos < len; pos += 2) {
                        v = *left + add;
                        if (byteswap)
                                v = bswap_16(v);
                        disko_write(fp, &v, 2);
                        left += stride;

                        v = *right + add;
                        if (byteswap)
                                v = bswap_16(v);
                        disko_write(fp, &v, 2);
                        right += stride;
                }

                if (len & 1) {
                        // odd length => write one more 'left'
                        v = *left + add;
                        if (byteswap)
                                v = bswap_16(v);
                        disko_write(fp, &v, 2);
                }

                len *= 2;
        } else {
                // 8-bit data. Mostly the same as above, but a little bit simpler since
                // there's no byteswapping, and the values can be written with putc.

                const int8_t *left, *right;
                left = (const int8_t *) sample->data;
                right = left + rightofs;

                // No point buffering the processing here -- the disk output already SHOULD have a 64kb buffer
                for (pos = 0; pos < len; pos += 2) {
                        disko_putc(fp, *left + add);
                        left += stride;

                        disko_putc(fp, *right + add);
                        right += stride;
                }

                if (len & 1) {
                        // odd length => write one more 'left'
                        disko_putc(fp, *left + add);
                }
        }

        return len;
}


uint32_t csf_read_sample(song_sample_t *sample, uint32_t flags, const void *filedata, uint32_t memsize)
{
        uint32_t len = 0, mem;
        const char *buffer = (const char *) filedata;

        // validate the read flags before anything else
        switch (flags & SF_BIT_MASK) {
                case SF_7: case SF_8: case SF_16: case SF_24: case SF_32: break;
                default: SF_FAIL("bit width", flags & SF_BIT_MASK);
        }
        switch (flags & SF_CHN_MASK) {
                case SF_M: case SF_SI: case SF_SS: break;
                default: SF_FAIL("channel mask", flags & SF_CHN_MASK);
        }
        switch (flags & SF_END_MASK) {
                case SF_LE: case SF_BE: break;
                default: SF_FAIL("endianness", flags & SF_END_MASK);
        }
        switch (flags & SF_ENC_MASK) {
                case SF_PCMS: case SF_PCMU: case SF_PCMD: case SF_IT214: case SF_IT215:
                case SF_AMS: case SF_DMF: case SF_MDL: case SF_PTM:
                        break;
                default: SF_FAIL("encoding", flags & SF_ENC_MASK);
        }
        if ((flags & ~(SF_BIT_MASK | SF_CHN_MASK | SF_END_MASK | SF_ENC_MASK)) != 0) {
                SF_FAIL("extra flag", flags & ~(SF_BIT_MASK | SF_CHN_MASK | SF_END_MASK | SF_ENC_MASK));
        }

        if (sample->flags & CHN_ADLIB) return 0; // no sample data

        if (!sample || sample->length < 1 || !buffer) return 0;
        if (sample->length > MAX_SAMPLE_LENGTH) sample->length = MAX_SAMPLE_LENGTH;
        mem = sample->length+6;
        sample->flags &= ~(CHN_16BIT|CHN_STEREO);
        if ((flags & SF_BIT_MASK) == SF_16) {
                mem *= 2;
                sample->flags |= CHN_16BIT;
        }
        switch (flags & SF_CHN_MASK) {
        case SF_SI: case SF_SS:
                mem *= 2;
                sample->flags |= CHN_STEREO;
        }
        if ((sample->data = csf_allocate_sample(mem)) == NULL) {
                sample->length = 0;
                return 0;
        }
        switch(flags) {
        // 1: 8-bit unsigned PCM data
        case RS_PCM8U:
                {
                        len = sample->length;
                        if (len > memsize) len = sample->length = memsize;
                        signed char *data = sample->data;
                        for (uint32_t j=0; j<len; j++) data[j] = (signed char)(buffer[j] - 0x80);
                }
                break;

        // 2: 8-bit ADPCM data with linear table
        case RS_PCM8D:
                {
                        len = sample->length;
                        if (len > memsize) break;
                        signed char *data = sample->data;
                        const signed char *p = (const signed char *)buffer;
                        int delta = 0;
                        for (uint32_t j=0; j<len; j++) {
                                delta += p[j];
                                *data++ = (signed char)delta;
                        }
                }
                break;

        // 4: 16-bit ADPCM data with linear table
        case RS_PCM16D:
                {
                        len = sample->length * 2;
                        if (len > memsize) break;
                        short *data = (short *)sample->data;
                        short *p = (short *)buffer;
                        unsigned short tmp;
                        int delta16 = 0;
                        for (uint32_t j=0; j<len; j+=2) {
                                tmp = *((unsigned short *)p++);
                                delta16 += bswapLE16(tmp);
                                *data++ = (short) delta16;
                        }
                }
                break;

        // 5: 16-bit signed PCM data
        case RS_PCM16S:
                {
                        len = sample->length * 2;
                        if (len <= memsize) memcpy(sample->data, buffer, len);
                        short int *data = (short int *)sample->data;
                        for (uint32_t j=0; j<len; j+=2) {
                                *data = bswapLE16(*data);
                                data++;
                        }
                }
                break;

        // 16-bit signed mono PCM motorola byte order
        case RS_PCM16M:
                len = sample->length * 2;
                if (len > memsize) len = memsize & ~1;
                if (len > 1) {
                        signed char *data = (signed char *)sample->data;
                        signed char *src = (signed char *)buffer;
                        for (uint32_t j=0; j<len; j+=2) {
                                // data[j] = src[j+1];
                                // data[j+1] = src[j];
                                *((unsigned short *)(data+j)) = bswapBE16(*((unsigned short *)(src+j)));
                        }
                }
                break;

        // 6: 16-bit unsigned PCM data
        case RS_PCM16U:
                {
                        len = sample->length * 2;
                        if (len <= memsize) memcpy(sample->data, buffer, len);
                        short int *data = (short int *)sample->data;
                        for (uint32_t j=0; j<len; j+=2) {
                                *data = bswapLE16(*data) - 0x8000;
                                data++;
                        }
                }
                break;

        // 16-bit signed stereo big endian
        case RS_STPCM16M:
                len = sample->length * 2;
                if (len*2 <= memsize) {
                        signed char *data = (signed char *)sample->data;
                        signed char *src = (signed char *)buffer;
                        for (uint32_t j=0; j<len; j+=2) {
                                // data[j*2] = src[j+1];
                                // data[j*2+1] = src[j];
                                // data[j*2+2] = src[j+1+len];
                                // data[j*2+3] = src[j+len];
                                *((unsigned short *)(data+j*2))
                                        = bswapBE16(*((unsigned short *)(src+j)));
                                *((unsigned short *)(data+j*2+2))
                                        = bswapBE16(*((unsigned short *)(src+j+len)));
                        }
                        len *= 2;
                }
                break;

        // 8-bit stereo samples
        case RS_STPCM8S:
        case RS_STPCM8U:
        case RS_STPCM8D:
                {
                        int iadd_l, iadd_r;
                        iadd_l = iadd_r = (flags == RS_STPCM8U) ? -128 : 0;
                        len = sample->length;
                        signed char *psrc = (signed char *)buffer;
                        signed char *data = (signed char *)sample->data;
                        if (len*2 > memsize) break;
                        for (uint32_t j=0; j<len; j++) {
                                data[j*2] = (signed char)(psrc[0] + iadd_l);
                                data[j*2+1] = (signed char)(psrc[len] + iadd_r);
                                psrc++;
                                if (flags == RS_STPCM8D) {
                                        iadd_l = data[j*2];
                                        iadd_r = data[j*2+1];
                                }
                        }
                        len *= 2;
                }
                break;

        // 16-bit stereo samples
        case RS_STPCM16S:
        case RS_STPCM16U:
        case RS_STPCM16D:
                {
                        int iadd_l, iadd_r;
                        iadd_l = iadd_r = (flags == RS_STPCM16U) ? -0x8000 : 0;
                        len = sample->length;
                        short int *psrc = (short int *)buffer;
                        short int *data = (short int *)sample->data;
                        if (len*4 > memsize) break;
                        for (uint32_t j=0; j<len; j++) {
                                data[j*2] = (short int) (bswapLE16(psrc[0]) + iadd_l);
                                data[j*2+1] = (short int) (bswapLE16(psrc[len]) + iadd_r);
                                psrc++;
                                if (flags == RS_STPCM16D) {
                                        iadd_l = data[j*2];
                                        iadd_r = data[j*2+1];
                                }
                        }
                        len *= 4;
                }
                break;

        // IT 2.14 compressed samples
        case RS_IT2148:
        case RS_IT21416:
        case RS_IT2158:
        case RS_IT21516:
                len = memsize;
                if (len < 2) break;
                if (flags == RS_IT2148 || flags == RS_IT2158) {
                        it_decompress8(sample->data, sample->length,
                                        buffer, memsize, (flags == RS_IT2158));
                } else {
                        it_decompress16(sample->data, sample->length,
                                        buffer, memsize, (flags == RS_IT21516));
                }
                break;

        // 8-bit interleaved stereo samples
        case RS_STIPCM8S:
        case RS_STIPCM8U:
                {
                        int iadd = 0;
                        if (flags == RS_STIPCM8U) { iadd = -0x80; }
                        len = sample->length;
                        if (len*2 > memsize) len = memsize >> 1;
                        uint8_t * psrc = (uint8_t *)buffer;
                        uint8_t * data = (uint8_t *)sample->data;
                        for (uint32_t j=0; j<len; j++) {
                                data[j*2] = (signed char)(psrc[0] + iadd);
                                data[j*2+1] = (signed char)(psrc[1] + iadd);
                                psrc+=2;
                        }
                        len *= 2;
                }
                break;

        // 16-bit interleaved stereo samples
        case RS_STIPCM16S:
        case RS_STIPCM16U:
                {
                        int iadd = 0;
                        if (flags == RS_STIPCM16U) iadd = -32768;
                        len = sample->length;
                        if (len*4 > memsize) len = memsize >> 2;
                        short int *psrc = (short int *)buffer;
                        short int *data = (short int *)sample->data;
                        for (uint32_t j=0; j<len; j++) {
                                data[j*2] = (short int)(bswapLE16(psrc[0]) + iadd);
                                data[j*2+1] = (short int)(bswapLE16(psrc[1]) + iadd);
                                psrc += 2;
                        }
                        len *= 4;
                }
                break;

#if 0
        // AMS compressed samples
        case RS_AMS8:
        case RS_AMS16:
                len = 9;
                if (memsize > 9) {
                        const char *psrc = buffer;
                        char packcharacter = buffer[8], *pdest = (char *)sample->data;
                        len += bswapLE32(*((uint32_t *)(buffer+4)));
                        if (len > memsize) len = memsize;
                        uint32_t dmax = sample->length;
                        if (sample->flags & CHN_16BIT) dmax <<= 1;
                        AMSUnpack(psrc+9, len-9, pdest, dmax, packcharacter);
                }
                break;
#endif

        // PTM 8bit delta to 16-bit sample
        case RS_PTM8DTO16:
                {
                        len = sample->length * 2;
                        if (len > memsize) break;
                        signed char *data = (signed char *)sample->data;
                        signed char delta8 = 0;
                        for (uint32_t j=0; j<len; j++) {
                                delta8 += buffer[j];
                                *data++ = delta8;
                        }
                        uint16_t *data16 = (uint16_t *)sample->data;
                        for (uint32_t j=0; j<len; j+=2) {
                                *data16 = bswapLE16(*data16);
                                data16++;
                        }
                }
                break;

        // Huffman MDL compressed samples
        case RS_MDL8:
        case RS_MDL16:
                if (memsize >= 8) {
                        // first 4 bytes indicate packed length
                        len = bswapLE32(*((uint32_t *) buffer));
                        len = MIN(len, memsize) + 4;
                        uint8_t * data = (uint8_t *)sample->data;
                        uint8_t * ibuf = (uint8_t *)(buffer + 4);
                        uint32_t bitbuf = bswapLE32(*((uint32_t *)ibuf));
                        uint32_t bitnum = 32;
                        uint8_t dlt = 0, lowbyte = 0;
                        ibuf += 4;
                        // TODO move all this junk to fmt/compression.c
                        for (uint32_t j=0; j<sample->length; j++) {
                                uint8_t hibyte;
                                uint8_t sign;
                                if (flags == RS_MDL16)
                                        lowbyte = (uint8_t)mdl_read_bits(&bitbuf, &bitnum, &ibuf, 8);
                                sign = (uint8_t)mdl_read_bits(&bitbuf, &bitnum, &ibuf, 1);
                                if (mdl_read_bits(&bitbuf, &bitnum, &ibuf, 1)) {
                                        hibyte = (uint8_t)mdl_read_bits(&bitbuf, &bitnum, &ibuf, 3);
                                } else {
                                        hibyte = 8;
                                        while (!mdl_read_bits(&bitbuf, &bitnum, &ibuf, 1)) hibyte += 0x10;
                                        hibyte += mdl_read_bits(&bitbuf, &bitnum, &ibuf, 4);
                                }
                                if (sign) hibyte = ~hibyte;
                                dlt += hibyte;
                                if (flags == RS_MDL8) {
                                        data[j] = dlt;
                                } else {
#ifdef WORDS_BIGENDIAN
                                        data[j<<1] = dlt;
                                        data[(j<<1)+1] = lowbyte;
#else
                                        data[j<<1] = lowbyte;
                                        data[(j<<1)+1] = dlt;
#endif
                                }
                        }
                }
                break;

#if 0
        case RS_DMF8:
        case RS_DMF16:
                len = memsize;
                if (len >= 4) {
                        uint32_t maxlen = sample->length;
                        if (sample->flags & CHN_16BIT) maxlen <<= 1;
                        uint8_t * ibuf = (uint8_t *)buffer;
                        uint8_t * ibufmax = (uint8_t *)(buffer+memsize);
                        len = DMFUnpack((uint8_t *)sample->data, ibuf, ibufmax, maxlen);
                }
                break;
#endif

#if 0 // THESE ARE BROKEN
        // PCM 24-bit signed -> load sample, and normalize it to 16-bit
        case RS_PCM24S:
        case RS_PCM32S:
                printf("PCM 24/32\n");
                len = sample->length * 3;
                if (flags == RS_PCM32S) len += sample->length;
                if (len > memsize) break;
                if (len > 4*8) {
                        uint32_t slsize = (flags == RS_PCM32S) ? 4 : 3;
                        uint8_t * src = (uint8_t *)buffer;
                        int32_t max = 255;
                        if (flags == RS_PCM32S) src++;
                        for (uint32_t j=0; j<len; j+=slsize) {
                                int32_t l = ((((src[j+2] << 8) + src[j+1]) << 8) + src[j]) << 8;
                                l /= 256;
                                if (l > max) max = l;
                                if (-l > max) max = -l;
                        }
                        max = (max / 128) + 1;
                        signed short *dest = (signed short *)sample->data;
                        for (uint32_t k=0; k<len; k+=slsize) {
                                int32_t l = ((((src[k+2] << 8) + src[k+1]) << 8) + src[k]) << 8;
                                *dest++ = (signed short)(l / max);
                        }
                }
                break;

        // Stereo PCM 24-bit signed -> load sample, and normalize it to 16-bit
        case RS_STIPCM24S:
        case RS_STIPCM32S:
                len = sample->length * 6;
                if (flags == RS_STIPCM32S) len += sample->length * 2;
                if (len > memsize) break;
                if (len > 8*8) {
                        uint32_t slsize = (flags == RS_STIPCM32S) ? 4 : 3;
                        uint8_t * src = (uint8_t *)buffer;
                        int32_t max = 255;
                        if (flags == RS_STIPCM32S) src++;
                        for (uint32_t j=0; j<len; j+=slsize) {
                                int32_t l = ((((src[j+2] << 8) + src[j+1]) << 8) + src[j]) << 8;
                                l /= 256;
                                if (l > max) max = l;
                                if (-l > max) max = -l;
                        }
                        max = (max / 128) + 1;
                        signed short *dest = (signed short *)sample->data;
                        for (uint32_t k=0; k<len; k+=slsize) {
                                int32_t lr = ((((src[k+2] << 8) + src[k+1]) << 8) + src[k]) << 8;
                                k += slsize;
                                int32_t ll = ((((src[k+2] << 8) + src[k+1]) << 8) + src[k]) << 8;
                                dest[0] = (signed short)ll;
                                dest[1] = (signed short)lr;
                                dest += 2;
                        }
                }
                break;
#endif

        // 16-bit signed big endian interleaved stereo
        case RS_STIPCM16M:
                {
                        len = sample->length;
                        if (len*4 > memsize) len = memsize >> 2;
                        const uint8_t * psrc = (const uint8_t *)buffer;
                        short int *data = (short int *)sample->data;
                        for (uint32_t j=0; j<len; j++) {
                                data[j*2] = (signed short)(((uint32_t)psrc[0] << 8) | (psrc[1]));
                                data[j*2+1] = (signed short)(((uint32_t)psrc[2] << 8) | (psrc[3]));
                                psrc += 4;
                        }
                        len *= 4;
                }
                break;

        // 7-bit (data shifted one bit left)
        case SF(7,M,BE,PCMS):
        case SF(7,M,LE,PCMS):
                sample->flags &= ~(CHN_16BIT | CHN_STEREO);
                len = sample->length = MIN(sample->length, memsize);
                for (uint32_t j = 0; j < len; j++)
                        sample->data[j] = CLAMP(buffer[j] * 2, -128, 127);
                break;

        // Default: 8-bit signed PCM data
        default:
                printf("DEFAULT: %d\n", flags);
        case SF(8,M,BE,PCMS): /* endianness is irrelevant for 8-bit samples */
        case SF(8,M,LE,PCMS):
                sample->flags &= ~(CHN_16BIT | CHN_STEREO);
                len = sample->length;
                if (len > memsize) len = sample->length = memsize;
                memcpy(sample->data, buffer, len);
                break;
        }
        if (len > memsize) {
                if (sample->data) {
                        sample->length = 0;
                        csf_free_sample(sample->data);
                        sample->data = NULL;
                }
                return 0;
        }
        csf_adjust_sample_loop(sample);
        return len;
}

/* --------------------------------------------------------------------------------------------------------- */

void csf_adjust_sample_loop(song_sample_t *sample)
{
        if (!sample->data) return;
        if (sample->loop_end > sample->length) sample->loop_end = sample->length;
        if (sample->loop_start+2 >= sample->loop_end) {
                sample->loop_start = sample->loop_end = 0;
                sample->flags &= ~CHN_LOOP;
        }

        // poopy, removing all that loop-hacking code has produced... very nasty sounding loops!
        // so I guess I should rewrite the crap at the end of the sample at least.
        uint32_t len = sample->length;
        if (sample->flags & CHN_16BIT) {
                short int *data = (short int *)sample->data;
                // Adjust end of sample
                if (sample->flags & CHN_STEREO) {
                        data[len*2+6]
                                = data[len*2+4]
                                = data[len*2+2]
                                = data[len*2]
                                = data[len*2-2];
                        data[len*2+7]
                                = data[len*2+5]
                                = data[len*2+3]
                                = data[len*2+1]
                                = data[len*2-1];
                } else {
                        data[len+4]
                                = data[len+3]
                                = data[len+2]
                                = data[len+1]
                                = data[len]
                                = data[len-1];
                }
        } else {
                signed char *data = sample->data;
                // Adjust end of sample
                if (sample->flags & CHN_STEREO) {
                        data[len*2+6]
                                = data[len*2+4]
                                = data[len*2+2]
                                = data[len*2]
                                = data[len*2-2];
                        data[len*2+7]
                                = data[len*2+5]
                                = data[len*2+3]
                                = data[len*2+1]
                                = data[len*2-1];
                } else {
                        data[len+4]
                                = data[len+3]
                                = data[len+2]
                                = data[len+1]
                                = data[len]
                                = data[len-1];
                }
        }
}


// FIXME this function sucks
uint32_t csf_get_highest_used_channel(song_t *csf)
{
        uint32_t highchan = 0;

        for (uint32_t ipat = 0; ipat < MAX_PATTERNS; ipat++) {
                song_note_t *p = csf->patterns[ipat];
                if (p) {
                        uint32_t jmax = csf->pattern_size[ipat] * MAX_CHANNELS;
                        for (uint32_t j = 0; j < jmax; j++, p++) {
                                if (NOTE_IS_NOTE(p->note)) {
                                        if ((j % MAX_CHANNELS) > highchan)
                                                highchan = j % MAX_CHANNELS;
                                }
                        }
                }
        }

        return highchan;
}



int csf_destroy_sample(song_t *csf, uint32_t smp)
{
        if (smp >= MAX_SAMPLES)
                return 0;
        if (!csf->samples[smp].data)
                return 1;
        song_sample_t *pins = &csf->samples[smp];
        signed char *data = pins->data;
        pins->data = NULL;
        pins->length = 0;
        pins->flags &= ~CHN_16BIT;
        for (uint32_t i=0; i<MAX_VOICES; i++) {
                if (csf->voices[i].data == data) {
                        csf->voices[i].position = csf->voices[i].length = 0;
                        csf->voices[i].data = csf->voices[i].current_sample_data = NULL;
                }
        }
        csf_free_sample(data);
        return 1;
}



void csf_import_mod_effect(song_note_t *m, int from_xm)
{
        uint32_t effect = m->effect, param = m->param;

        switch(effect) {
        case 0x00:      if (param) effect = FX_ARPEGGIO; break;
        case 0x01:      effect = FX_PORTAMENTOUP; break;
        case 0x02:      effect = FX_PORTAMENTODOWN; break;
        case 0x03:      effect = FX_TONEPORTAMENTO; break;
        case 0x04:      effect = FX_VIBRATO; break;
        case 0x05:      effect = FX_TONEPORTAVOL; if (param & 0xF0) param &= 0xF0; break;
        case 0x06:      effect = FX_VIBRATOVOL; if (param & 0xF0) param &= 0xF0; break;
        case 0x07:      effect = FX_TREMOLO; break;
        case 0x08:
                effect = FX_PANNING;
                if (!from_xm)
                        param = MAX(param * 2, 0xff);
                break;
        case 0x09:      effect = FX_OFFSET; break;
        case 0x0A:      effect = FX_VOLUMESLIDE; if (param & 0xF0) param &= 0xF0; break;
        case 0x0B:      effect = FX_POSITIONJUMP; break;
        case 0x0C:
                if (from_xm) {
                        effect = FX_VOLUME;
                } else {
                        m->voleffect = VOLFX_VOLUME;
                        m->volparam = param;
                        if (m->voleffect > 64)
                                m->voleffect = 64;
                        effect = param = 0;
                }
                break;
        case 0x0D:      effect = FX_PATTERNBREAK; param = ((param >> 4) * 10) + (param & 0x0F); break;
        case 0x0E:
                effect = FX_S3MCMDEX;
                switch(param & 0xF0) {
                        case 0x10: effect = FX_PORTAMENTOUP; param |= 0xF0; break;
                        case 0x20: effect = FX_PORTAMENTODOWN; param |= 0xF0; break;
                        case 0x30: param = (param & 0x0F) | 0x10; break;
                        case 0x40: param = (param & 0x0F) | 0x30; break;
                        case 0x50: param = (param & 0x0F) | 0x20; break;
                        case 0x60: param = (param & 0x0F) | 0xB0; break;
                        case 0x70: param = (param & 0x0F) | 0x40; break;
                        case 0x90: effect = FX_RETRIG; param &= 0x0F; break;
                        case 0xA0:
                                if (param & 0x0F) {
                                        effect = FX_VOLUMESLIDE;
                                        param = (param << 4) | 0x0F;
                                } else {
                                        effect = param = 0;
                                }
                                break;
                        case 0xB0:
                                if (param & 0x0F) {
                                        effect = FX_VOLUMESLIDE;
                                        param |= 0xF0;
                                } else {
                                        effect=param=0;
                                }
                                break;
                }
                break;
        case 0x0F:
                // FT2 processes 0x20 as Txx; ST3 loads it as Axx
                effect = (param < (from_xm ? 0x20 : 0x21)) ? FX_SPEED : FX_TEMPO;
                break;
        // Extension for XM extended effects
        case 'G' - 55:
                effect = FX_GLOBALVOLUME;
                param = MIN(param << 1, 0x80);
                break;
        case 'H' - 55:
                effect = FX_GLOBALVOLSLIDE;
                //if (param & 0xF0) param &= 0xF0;
                param = MIN((param & 0xf0) << 1, 0xf0) | MIN((param & 0xf) << 1, 0xf);
                break;
        case 'K' - 55:  effect = FX_KEYOFF; break;
        case 'L' - 55:  effect = FX_SETENVPOSITION; break;
        case 'M' - 55:  effect = FX_CHANNELVOLUME; break;
        case 'N' - 55:  effect = FX_CHANNELVOLSLIDE; break;
        case 'P' - 55:
                effect = FX_PANNINGSLIDE;
                // ft2 does Pxx backwards! skjdfjksdfkjsdfjk
                if (param & 0xF0)
                        param >>= 4;
                else
                        param = (param & 0xf) << 4;
                break;
        case 'R' - 55:  effect = FX_RETRIG; break;
        case 'T' - 55:  effect = FX_TREMOR; break;
        case 'X' - 55:
                switch (param & 0xf0) {
                case 0x10:
                        effect = FX_PORTAMENTOUP;
                        param = 0xe0 | (param & 0xf);
                        break;
                case 0x20:
                        effect = FX_PORTAMENTODOWN;
                        param = 0xe0 | (param & 0xf);
                        break;
                default:
                        effect = param = 0;
                        break;
                }
                break;
        case 'Y' - 55:  effect = FX_PANBRELLO; break;
        case 'Z' - 55:  effect = FX_MIDI;     break;
        case '[' - 55:
                // FT2 shows this weird effect as -xx, and it can even be inserted
                // by typing "-", although it doesn't appear to do anything.
        default:        effect = 0;
        }
        m->effect = effect;
        m->param = param;
}

uint16_t csf_export_mod_effect(const song_note_t *m, int bXM)
{
        uint32_t effect = m->effect & 0x3F, param = m->param;

        switch(effect) {
        case 0:                         effect = param = 0; break;
        case FX_ARPEGGIO:              effect = 0; break;
        case FX_PORTAMENTOUP:
                if ((param & 0xF0) == 0xE0) {
                        if (bXM) {
                                effect = 'X' - 55;
                                param = 0x10 | (param & 0xf);
                        } else {
                                effect = 0x0E;
                                param = 0x10 | ((param & 0xf) >> 2);
                        }
                } else if ((param & 0xF0) == 0xF0) {
                        effect = 0x0E;
                        param = 0x10 | (param & 0xf);
                } else {
                        effect = 0x01;
                }
                break;
        case FX_PORTAMENTODOWN:
                if ((param & 0xF0) == 0xE0) {
                        if (bXM) {
                                effect = 'X' - 55;
                                param = 0x20 | (param & 0xf);
                        } else {
                                effect = 0x0E;
                                param = 0x20 | ((param & 0xf) >> 2);
                        }
                } else if ((param & 0xF0) == 0xF0) {
                        effect = 0x0E;
                        param = 0x20 | (param & 0xf);
                } else {
                        effect = 0x02;
                }
                break;
        case FX_TONEPORTAMENTO:        effect = 0x03; break;
        case FX_VIBRATO:               effect = 0x04; break;
        case FX_TONEPORTAVOL:          effect = 0x05; break;
        case FX_VIBRATOVOL:            effect = 0x06; break;
        case FX_TREMOLO:               effect = 0x07; break;
        case FX_PANNING:
                effect = 0x08;
                if (!bXM) param >>= 1;
                break;
        case FX_OFFSET:                effect = 0x09; break;
        case FX_VOLUMESLIDE:           effect = 0x0A; break;
        case FX_POSITIONJUMP:          effect = 0x0B; break;
        case FX_VOLUME:                effect = 0x0C; break;
        case FX_PATTERNBREAK:          effect = 0x0D; param = ((param / 10) << 4) | (param % 10); break;
        case FX_SPEED:                 effect = 0x0F; if (param > 0x20) param = 0x20; break;
        case FX_TEMPO:                 if (param > 0x20) { effect = 0x0F; break; } return 0;
        case FX_GLOBALVOLUME:          effect = 'G' - 55; break;
        case FX_GLOBALVOLSLIDE:        effect = 'H' - 55; break; // FIXME this needs to be adjusted
        case FX_KEYOFF:                effect = 'K' - 55; break;
        case FX_SETENVPOSITION:        effect = 'L' - 55; break;
        case FX_CHANNELVOLUME:         effect = 'M' - 55; break;
        case FX_CHANNELVOLSLIDE:       effect = 'N' - 55; break;
        case FX_PANNINGSLIDE:          effect = 'P' - 55; break;
        case FX_RETRIG:                effect = 'R' - 55; break;
        case FX_TREMOR:                effect = 'T' - 55; break;
        case FX_PANBRELLO:             effect = 'Y' - 55; break;
        case FX_MIDI:                  effect = 'Z' - 55; break;
        case FX_S3MCMDEX:
                switch (param & 0xF0) {
                case 0x10:      effect = 0x0E; param = (param & 0x0F) | 0x30; break;
                case 0x20:      effect = 0x0E; param = (param & 0x0F) | 0x50; break;
                case 0x30:      effect = 0x0E; param = (param & 0x0F) | 0x40; break;
                case 0x40:      effect = 0x0E; param = (param & 0x0F) | 0x70; break;
                case 0x90:      effect = 'X' - 55; break;
                case 0xB0:      effect = 0x0E; param = (param & 0x0F) | 0x60; break;
                case 0xA0:
                case 0x50:
                case 0x70:
                case 0x60:      effect = param = 0; break;
                default:        effect = 0x0E; break;
                }
                break;
        default:                effect = param = 0;
        }
        return (uint16_t)((effect << 8) | (param));
}


void csf_import_s3m_effect(song_note_t *m, int bIT)
{
        uint32_t effect = m->effect;
        uint32_t param = m->param;
        switch (effect + 0x40)
        {
        case 'A':       effect = FX_SPEED; break;
        case 'B':       effect = FX_POSITIONJUMP; break;
        case 'C':
                effect = FX_PATTERNBREAK;
                if (!bIT)
                        param = (param >> 4) * 10 + (param & 0x0F);
                break;
        case 'D':       effect = FX_VOLUMESLIDE; break;
        case 'E':       effect = FX_PORTAMENTODOWN; break;
        case 'F':       effect = FX_PORTAMENTOUP; break;
        case 'G':       effect = FX_TONEPORTAMENTO; break;
        case 'H':       effect = FX_VIBRATO; break;
        case 'I':       effect = FX_TREMOR; break;
        case 'J':       effect = FX_ARPEGGIO; break;
        case 'K':       effect = FX_VIBRATOVOL; break;
        case 'L':       effect = FX_TONEPORTAVOL; break;
        case 'M':       effect = FX_CHANNELVOLUME; break;
        case 'N':       effect = FX_CHANNELVOLSLIDE; break;
        case 'O':       effect = FX_OFFSET; break;
        case 'P':       effect = FX_PANNINGSLIDE; break;
        case 'Q':       effect = FX_RETRIG; break;
        case 'R':       effect = FX_TREMOLO; break;
        case 'S':
                effect = FX_S3MCMDEX;
                // convert old SAx to S8x
                if (!bIT && ((param & 0xf0) == 0xa0))
                        param = 0x80 | ((param & 0xf) ^ 8);
                break;
        case 'T':       effect = FX_TEMPO; break;
        case 'U':       effect = FX_FINEVIBRATO; break;
        case 'V':
                effect = FX_GLOBALVOLUME;
                if (!bIT)
                        param *= 2;
                break;
        case 'W':       effect = FX_GLOBALVOLSLIDE; break;
        case 'X':
                effect = FX_PANNING;
                if (!bIT) {
                        if (param == 0xa4) {
                                effect = FX_S3MCMDEX;
                                param = 0x91;
                        } else if (param > 0x7f) {
                                param = 0xff;
                        } else {
                                param *= 2;
                        }
                }
                break;
        case 'Y':       effect = FX_PANBRELLO; break;
        case 'Z':       effect = FX_MIDI; break;
        default:        effect = 0;
        }
        m->effect = effect;
        m->param = param;
}

void csf_export_s3m_effect(uint32_t *pcmd, uint32_t *pprm, int bIT)
{
        uint32_t effect = *pcmd;
        uint32_t param = *pprm;
        switch (effect) {
        case FX_SPEED:                 effect = 'A'; break;
        case FX_POSITIONJUMP:          effect = 'B'; break;
        case FX_PATTERNBREAK:          effect = 'C';
                                        if (!bIT) param = ((param / 10) << 4) + (param % 10); break;
        case FX_VOLUMESLIDE:           effect = 'D'; break;
        case FX_PORTAMENTODOWN:        effect = 'E'; break;
        case FX_PORTAMENTOUP:          effect = 'F'; break;
        case FX_TONEPORTAMENTO:        effect = 'G'; break;
        case FX_VIBRATO:               effect = 'H'; break;
        case FX_TREMOR:                effect = 'I'; break;
        case FX_ARPEGGIO:              effect = 'J'; break;
        case FX_VIBRATOVOL:            effect = 'K'; break;
        case FX_TONEPORTAVOL:          effect = 'L'; break;
        case FX_CHANNELVOLUME:         effect = 'M'; break;
        case FX_CHANNELVOLSLIDE:       effect = 'N'; break;
        case FX_OFFSET:                effect = 'O'; break;
        case FX_PANNINGSLIDE:          effect = 'P'; break;
        case FX_RETRIG:                effect = 'Q'; break;
        case FX_TREMOLO:               effect = 'R'; break;
        case FX_S3MCMDEX:
                if (!bIT && param == 0x91) {
                        effect = 'X';
                        param = 0xA4;
                } else {
                        effect = 'S';
                }
                break;
        case FX_TEMPO:                 effect = 'T'; break;
        case FX_FINEVIBRATO:           effect = 'U'; break;
        case FX_GLOBALVOLUME:          effect = 'V'; if (!bIT) param >>= 1;break;
        case FX_GLOBALVOLSLIDE:        effect = 'W'; break;
        case FX_PANNING:
                effect = 'X';
                if (!bIT)
                        param >>= 1;
                break;
        case FX_PANBRELLO:             effect = 'Y'; break;
        case FX_MIDI:                  effect = 'Z'; break;
        default:        effect = 0;
        }
        effect &= ~0x40;
        *pcmd = effect;
        *pprm = param;
}


void csf_insert_restart_pos(song_t *csf, uint32_t restart_order)
{
        int n, max, row;
        int ord, pat, newpat;
        int used; // how many times it was used (if >1, copy it)

        if (!restart_order)
                return;

        // find the last pattern, also look for one that's not being used
        for (max = ord = n = 0; n < MAX_ORDERS && csf->orderlist[n] < MAX_PATTERNS; ord = n, n++)
                if (csf->orderlist[n] > max)
                        max = csf->orderlist[n];
        newpat = max + 1;
        pat = csf->orderlist[ord];
        if (pat >= MAX_PATTERNS || !csf->patterns[pat] || !csf->pattern_size[pat])
                return;
        for (max = n, used = 0, n = 0; n < max; n++)
                if (csf->orderlist[n] == pat)
                        used++;

        if (used > 1) {
                // copy the pattern so we don't screw up the playback elsewhere
                while (newpat < MAX_PATTERNS && csf->patterns[newpat])
                        newpat++;
                if (newpat >= MAX_PATTERNS)
                        return; // no more patterns? sux
                //log_appendf(2, "Copying pattern %d to %d for restart position", pat, newpat);
                csf->patterns[newpat] = csf_allocate_pattern(csf->pattern_size[pat]);
                csf->pattern_size[newpat] = csf->pattern_alloc_size[newpat] = csf->pattern_size[pat];
                memcpy(csf->patterns[newpat], csf->patterns[pat],
                        sizeof(song_note_t) * MAX_CHANNELS * csf->pattern_size[pat]);
                csf->orderlist[ord] = pat = newpat;
        } else {
                //log_appendf(2, "Modifying pattern %d to add restart position", pat);
        }


        max = csf->pattern_size[pat] - 1;
        for (row = 0; row <= max; row++) {
                song_note_t *note = csf->patterns[pat] + MAX_CHANNELS * row;
                song_note_t *empty = NULL; // where's an empty effect?
                int has_break = 0, has_jump = 0;

                for (n = 0; n < MAX_CHANNELS; n++, note++) {
                        switch (note->effect) {
                        case FX_POSITIONJUMP:
                                has_jump = 1;
                                break;
                        case FX_PATTERNBREAK:
                                has_break = 1;
                                if (!note->param)
                                        empty = note; // always rewrite C00 with Bxx (it's cleaner)
                                break;
                        case FX_NONE:
                                if (!empty)
                                        empty = note;
                                break;
                        }
                }

                // if there's not already a Bxx, and we have a spare channel,
                // AND either there's a Cxx or it's the last row of the pattern,
                // then stuff in a jump back to the restart position.
                if (!has_jump && empty && (has_break || row == max)) {
                        empty->effect = FX_POSITIONJUMP;
                        empty->param = restart_order;
                }
        }
}
