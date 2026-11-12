/* Copyright (C) 2007 Jean-Marc Valin

   File: testresample2.c
   Testing the resampling code

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
   STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_resampler.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PERIOD 32
#define INBLOCK 1024
#define RATE 48000

/* Multi-stream mixing test: several streams at different sampling rates are
   resampled and mixed into a single stream. The mixed result is compared
   sample-by-sample against resampling every stream on its own. */
#define MIX_STREAMS 8
#define MIX_OUT_RATE 48000
#define MIX_QUALITY 4
#define MIX_CHUNK 97
#define MIX_OUT_CHUNK 1024

static const spx_uint32_t mix_rates[MIX_STREAMS] =
   {8000, 16000, 22050, 32000, 44100, 44100, 32000, 16000};

static float *mix_inputs[MIX_STREAMS];
static spx_uint32_t mix_in_len[MIX_STREAMS];

static void mix_generate_inputs(void)
{
   int s;
   spx_uint32_t i;
   for (s=0;s<MIX_STREAMS;s++)
   {
      const float freq = 440.f*(s+1);
      mix_in_len[s] = mix_rates[s]/10; /* 100 ms per stream: small assets */
      mix_inputs[s] = malloc(mix_in_len[s]*sizeof(float));
      for (i=0;i<mix_in_len[s];i++)
         mix_inputs[s][i] = 0.1f*sinf(2.f*(float)M_PI*freq*i/mix_rates[s])
                          + 0.05f*sinf(2.f*(float)M_PI*1.5f*freq*i/mix_rates[s]);
   }
}

/* Reference: resample every stream with its own resampler, then sum */
static spx_uint32_t mix_reference(float *ref_mix)
{
   int s;
   spx_uint32_t j, max_len = 0;
   for (s=0;s<MIX_STREAMS;s++)
   {
      float *ref = malloc((mix_in_len[s]*MIX_OUT_RATE/mix_rates[s]+64)*sizeof(float));
      spx_uint32_t in_len = mix_in_len[s];
      spx_uint32_t out_len = mix_in_len[s]*MIX_OUT_RATE/mix_rates[s]+64;
      SpeexResamplerState *st = speex_resampler_init(1, mix_rates[s], MIX_OUT_RATE, MIX_QUALITY, NULL);
      speex_resampler_skip_zeros(st);
      speex_resampler_process_float(st, 0, mix_inputs[s], &in_len, ref, &out_len);
      speex_resampler_destroy(st);
      if (out_len > max_len)
         max_len = out_len;
      for (j=0;j<out_len;j++)
         ref_mix[j] += ref[j];
      free(ref);
   }
   return max_len;
}

static int mix_check(const char *dump_file)
{
   float *ref_mix, *mix;
   spx_uint32_t ref_len, mix_len = 0, j;
   spx_uint32_t pos[MIX_STREAMS];
   const float *chunk_in[MIX_STREAMS];
   spx_uint32_t chunk_len[MIX_STREAMS];
   double max_diff = 0;
   int s, ret = 1, active;
   SpeexResamplerMixer *mx;

   mix_generate_inputs();
   ref_mix = calloc(MIX_OUT_RATE, sizeof(float));
   mix = calloc(MIX_OUT_RATE, sizeof(float));
   ref_len = mix_reference(ref_mix);

   /* Mixer: same inputs, fed in small interleaved chunks */
   mx = speex_resampler_mixer_init(MIX_OUT_RATE, MIX_QUALITY, MIX_STREAMS, NULL);
   for (s=0;s<MIX_STREAMS;s++)
   {
      int id = speex_resampler_mixer_add_stream(mx, mix_rates[s]);
      if (id != s)
      {
         fprintf(stderr, "add_stream failed for stream %d\n", s);
         goto out;
      }
      pos[s] = 0;
   }
   do {
      spx_uint32_t out_len = MIX_OUT_CHUNK;
      active = 0;
      for (s=0;s<MIX_STREAMS;s++)
      {
         spx_uint32_t remain = mix_in_len[s]-pos[s];
         /* Offer each stream as much input as it may need to fill this
            output block; the resampler only consumes what it needs, so all
            streams stay aligned on the common output timeline. */
         spx_uint32_t need = (spx_uint32_t)(((long long)MIX_OUT_CHUNK*mix_rates[s]+MIX_OUT_RATE-1)/MIX_OUT_RATE)+64;
         chunk_len[s] = remain < need ? remain : need;
         chunk_in[s] = mix_inputs[s]+pos[s];
         if (chunk_len[s])
            active = 1;
      }
      if (!active)
         break;
      speex_resampler_mixer_process_float(mx, chunk_in, chunk_len, mix+mix_len, &out_len);
      for (s=0;s<MIX_STREAMS;s++)
         pos[s] += chunk_len[s];
      mix_len += out_len;
   } while (1);
   speex_resampler_mixer_destroy(mx);

   if (mix_len != ref_len)
   {
      fprintf(stderr, "length mismatch: mixer %u vs reference %u\n", mix_len, ref_len);
      goto out;
   }
   for (j=0;j<mix_len;j++)
   {
      double d = fabs((double)mix[j]-ref_mix[j]);
      if (d > max_diff)
         max_diff = d;
   }
   printf("[check] %d streams mixed, %u output samples, max sample diff vs per-stream: %g\n",
          MIX_STREAMS, mix_len, max_diff);
   if (max_diff == 0)
      printf("[check] PASS: multi-stream mix is sample-by-sample identical to per-stream processing\n");
   else
      printf("[check] FAIL: outputs differ\n");
   ret = max_diff != 0;

   if (dump_file)
   {
      FILE *f = fopen(dump_file, "wb");
      if (f)
      {
         fwrite(mix, sizeof(float), mix_len, f);
         fclose(f);
         printf("[check] mixed output (%u samples, float32) written to %s\n", mix_len, dump_file);
      }
   }
out:
   free(ref_mix);
   free(mix);
   for (s=0;s<MIX_STREAMS;s++)
      free(mix_inputs[s]);
   return ret;
}

static int mix_limit(void)
{
   int ret = 1;
   SpeexResamplerMixer *mx = speex_resampler_mixer_init(MIX_OUT_RATE, MIX_QUALITY, 2, NULL);
   int id0 = speex_resampler_mixer_add_stream(mx, 8000);
   int id1 = speex_resampler_mixer_add_stream(mx, 16000);
   int id2 = speex_resampler_mixer_add_stream(mx, 44100);
   printf("[limit] max_streams=2, add_stream returned %d, %d, %d", id0, id1, id2);
   if (id0 == 0 && id1 == 1 && id2 == -RESAMPLER_ERR_TOO_MANY_STREAMS)
   {
      printf(" -> PASS: third stream rejected (%s)\n", speex_resampler_strerror(-id2));
      ret = 0;
   } else {
      printf(" -> FAIL\n");
   }
   speex_resampler_mixer_destroy(mx);
   return ret;
}

static size_t mix_heap_usage(void)
{
#ifdef __GLIBC__
   return mallinfo2().uordblks;
#else
   return 0;
#endif
}

static int mix_mem(void)
{
   int k, s;
   struct rusage ru;
   printf("[mem]  streams | mixer heap (B) | naive per-stream heap (B)\n");
   for (k=1;k<=MIX_STREAMS;k++)
   {
      size_t before, mixer_bytes, naive_bytes;
      SpeexResamplerMixer *mx;
      SpeexResamplerState *st[MIX_STREAMS];
      float *buf[MIX_STREAMS];
      spx_uint32_t in_len[MIX_STREAMS], out_len = MIX_OUT_CHUNK;
      float out[MIX_OUT_CHUNK];

      before = mix_heap_usage();
      mx = speex_resampler_mixer_init(MIX_OUT_RATE, MIX_QUALITY, k, NULL);
      for (s=0;s<k;s++)
         speex_resampler_mixer_add_stream(mx, mix_rates[s]);
      for (s=0;s<k;s++)
         in_len[s] = MIX_CHUNK;
      speex_resampler_mixer_process_float(mx, NULL, in_len, out, &out_len);
      mixer_bytes = mix_heap_usage()-before;
      speex_resampler_mixer_destroy(mx);

      before = mix_heap_usage();
      for (s=0;s<k;s++)
      {
         st[s] = speex_resampler_init(1, mix_rates[s], MIX_OUT_RATE, MIX_QUALITY, NULL);
         speex_resampler_skip_zeros(st[s]);
         buf[s] = malloc(MIX_OUT_CHUNK*sizeof(float));
      }
      naive_bytes = mix_heap_usage()-before;
      for (s=0;s<k;s++)
      {
         speex_resampler_destroy(st[s]);
         free(buf[s]);
      }
      printf("[mem] %8d | %14lu | %25lu\n", k,
             (unsigned long)mixer_bytes, (unsigned long)naive_bytes);
   }
   getrusage(RUSAGE_SELF, &ru);
   printf("[mem] peak process memory (ru_maxrss): %ld KB\n", ru.ru_maxrss);
   return 0;
}

static int test_mixer(int argc, char **argv)
{
   const char *mode = argc > 2 ? argv[2] : "all";
   const char *dump_file = argc > 3 ? argv[3] : NULL;
   int ret = 0;
   if (!strcmp(mode, "check") || !strcmp(mode, "all"))
      ret |= mix_check(dump_file);
   if (!strcmp(mode, "limit") || !strcmp(mode, "all"))
      ret |= mix_limit();
   if (!strcmp(mode, "mem") || !strcmp(mode, "all"))
      ret |= mix_mem();
   return ret;
}

int main(int argc, char **argv)
{
   spx_uint32_t i;
   float *fin, *fout;
   int rate = 1000, off = 0, avail = INBLOCK;

   if (argc > 1 && !strcmp(argv[1], "mix"))
      return test_mixer(argc, argv);

   SpeexResamplerState *st = speex_resampler_init(1, RATE, RATE, 4, NULL);
   speex_resampler_set_rate(st, RATE, rate);
   speex_resampler_skip_zeros(st);

   fin = malloc(INBLOCK*2*sizeof(float));
   for (i=0; i<INBLOCK*2;i++)
     fin[i] = sinf ((float)i/PERIOD * 2 * M_PI) * 0.9;

   fout = malloc(INBLOCK*4*sizeof(float));

   while (1)
   {
      spx_uint32_t in_len;
      spx_uint32_t out_len;

      in_len = avail;
      out_len = (in_len * rate + RATE-1) / RATE;

      fprintf (stderr, "%d %d %d %d -> ", rate, off, in_len, out_len);

      speex_resampler_process_float(st, 0, fin + off, &in_len, fout, &out_len);

      fprintf (stderr, "%d %d\n", in_len, out_len);
      off += in_len;
      avail = avail - in_len + INBLOCK;

      if (off >= INBLOCK)
        off -= INBLOCK;

      fwrite(fout, sizeof(float), out_len, stdout);

      rate += 100;
      if (rate > 128000)
        break;

      speex_resampler_set_rate(st, RATE, rate);
   }
   speex_resampler_destroy(st);
   free(fin);
   free(fout);
   return 0;
}
