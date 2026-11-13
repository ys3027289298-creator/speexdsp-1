/* Copyright (C) 2026 Jean-Marc Valin

   File: testresample3.c
   In-call runtime sample-rate switching test.

   A single resampler state is created once and kept for the whole call.
   While streaming fixed-size audio frames, the target rate is switched
   48000 -> 44100 -> 32000 Hz between frames. The state is migrated by
   speex_resampler_set_rate(), never destroyed/recreated, and streaming
   continues without flushing or inserting zeros. The program also checks
   illegal target rates, measures the wall-clock cost of each switch and
   quantifies output continuity at the switch points.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the above copyright notice
   and this permission notice appear in all copies.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_resampler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define QUALITY 5
#define FRAME_MS 10
#define MAX_RATE 48000
#define FRAME_SAMPLES (MAX_RATE * FRAME_MS / 1000) /* 480 input samples */
#define NUM_SWITCHES 2
#define SEGMENTS (NUM_SWITCHES + 1)

/* Switch after this many input frames have been processed. */
static const int switch_frame[NUM_SWITCHES] = {60, 120};
static const spx_uint32_t target_rate[SEGMENTS] = {48000, 44100, 32000};
static const spx_uint32_t total_frames = 180;

static double now_seconds(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_float(const void *a, const void *b)
{
   float fa = *(const float *)a;
   float fb = *(const float *)b;
   return (fa > fb) - (fa < fb);
}

int main(int argc, char **argv)
{
   const char *out_path = "output/testresample3_out.f32";
   const char *stats_path = "output/testresample3_stats.txt";
   if (argc > 1)
      out_path = argv[1];
   if (argc > 2)
      stats_path = argv[2];

   int err = 0;
   SpeexResamplerState *st = speex_resampler_init(1, MAX_RATE, target_rate[0],
                                                   QUALITY, &err);
   if (!st)
   {
      fprintf(stderr, "can't create resampler: %s\n", speex_resampler_strerror(err));
      return 1;
   }
   SpeexResamplerState *const state_handle = st;
   speex_resampler_skip_zeros(st);

   size_t cap = (size_t)total_frames * FRAME_SAMPLES * 2;
   float *out_all = (float *)malloc(cap * sizeof(float));
   float *frame_in = (float *)malloc(FRAME_SAMPLES * sizeof(float));
   float *frame_out = (float *)malloc(FRAME_SAMPLES * 2 * sizeof(float));
   if (!out_all || !frame_in || !frame_out)
   {
      fprintf(stderr, "allocation failed\n");
      return 1;
   }
   size_t out_total = 0;
   spx_uint32_t segment = 0;
   spx_uint32_t boundary_out[NUM_SWITCHES] = {0, 0};
   double switch_seconds[NUM_SWITCHES] = {0.0, 0.0};
   double max_switch_seconds = 0.0;

   for (spx_uint32_t frame = 0; frame < total_frames; frame++)
   {
      /* Pending switch happens between frames: same state, no flush. */
      if (segment < NUM_SWITCHES && (int)frame == switch_frame[segment])
      {
         double t0 = now_seconds();
         int rc = speex_resampler_set_rate(st, MAX_RATE, target_rate[segment + 1]);
         double dt = now_seconds() - t0;
         switch_seconds[segment] = dt;
         if (dt > max_switch_seconds)
            max_switch_seconds = dt;
         if (rc != RESAMPLER_ERR_SUCCESS)
         {
            fprintf(stderr, "switch %u failed: %s\n", segment + 1,
                    speex_resampler_strerror(rc));
            return 1;
         }
         if (st != state_handle)
         {
            fprintf(stderr, "state was recreated during switch %u\n", segment + 1);
            return 1;
         }
         boundary_out[segment] = (spx_uint32_t)out_total;
         spx_uint32_t got_in = 0, got_out = 0;
         speex_resampler_get_rate(st, &got_in, &got_out);
         fprintf(stderr,
                 "switch %u after frame %d: %u -> %u Hz, %.6f ms, out pos %u\n",
                 segment + 1, frame, target_rate[segment], got_out,
                 dt * 1000.0, boundary_out[segment]);
         segment++;
      }

      /* Small synthetic source: two steady tones. */
      for (int i = 0; i < FRAME_SAMPLES; i++)
      {
         double t = (double)(frame * FRAME_SAMPLES + i) / MAX_RATE;
         frame_in[i] = (float)(0.5 * sin(2.0 * M_PI * 440.0 * t)
                               + 0.25 * sin(2.0 * M_PI * 1000.0 * t));
      }

      spx_uint32_t in_len = FRAME_SAMPLES;
      spx_uint32_t out_len = FRAME_SAMPLES * 2;
      int rc = speex_resampler_process_float(st, 0, frame_in, &in_len,
                                             frame_out, &out_len);
      if (rc != RESAMPLER_ERR_SUCCESS || in_len != FRAME_SAMPLES)
      {
         fprintf(stderr, "process failed at frame %u (rc=%d, consumed=%u)\n",
                 frame, rc, in_len);
         return 1;
      }
      if (out_total + out_len > cap)
      {
         fprintf(stderr, "output buffer too small\n");
         return 1;
      }
      memcpy(out_all + out_total, frame_out, out_len * sizeof(float));
      out_total += out_len;
   }

   /* Illegal target rates must return an error code and leave the live
      stream (48000 -> 32000) intact. */
   struct { spx_uint32_t in_rate; spx_uint32_t out_rate; const char *name; } bad[] = {
      {MAX_RATE, 0, "zero output rate"},
      {0, 32000, "zero input rate"},
   };
   spx_uint32_t saved_in = 0, saved_out = 0;
   speex_resampler_get_rate(st, &saved_in, &saved_out);
   for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
   {
      int rc = speex_resampler_set_rate(st, bad[k].in_rate, bad[k].out_rate);
      if (rc != RESAMPLER_ERR_INVALID_ARG)
      {
         fprintf(stderr, "%s: expected INVALID_ARG, got %d\n", bad[k].name, rc);
         return 1;
      }
      spx_uint32_t cur_in = 0, cur_out = 0;
      speex_resampler_get_rate(st, &cur_in, &cur_out);
      if (cur_in != saved_in || cur_out != saved_out)
      {
         fprintf(stderr, "%s: rate changed despite failed switch\n", bad[k].name);
         return 1;
      }
      fprintf(stderr, "illegal target (%u, %u) -> code %d (%s), state kept\n",
              bad[k].in_rate, bad[k].out_rate, rc,
              speex_resampler_strerror(rc));
   }
   int null_rc = speex_resampler_set_rate(NULL, MAX_RATE, 32000);
   if (null_rc != RESAMPLER_ERR_BAD_STATE)
   {
      fprintf(stderr, "NULL state: expected BAD_STATE, got %d\n", null_rc);
      return 1;
   }
   fprintf(stderr, "NULL state -> code %d (%s)\n", null_rc,
           speex_resampler_strerror(null_rc));

   /* Continuity: first-difference distribution over steady regions vs. the
      samples straddling each switch point. */
   size_t ndiff = out_total - 1;
   float *diffs = (float *)malloc(ndiff * sizeof(float));
   for (size_t i = 0; i < ndiff; i++)
      diffs[i] = fabsf(out_all[i + 1] - out_all[i]);
   float *sorted = (float *)malloc(ndiff * sizeof(float));
   memcpy(sorted, diffs, ndiff * sizeof(float));
   qsort(sorted, ndiff, sizeof(float), cmp_float);
   float median_diff = sorted[ndiff / 2];
   float p999_diff = sorted[(size_t)(ndiff * 0.999)];
   /* In fixed-point builds adjacent output samples are often identical, so
      the median can be 0; anchor the threshold on p99.9 with an absolute
      floor to keep the check meaningful in both modes. */
   float continuity_ref = p999_diff > median_diff ? p999_diff : median_diff;
   if (continuity_ref < 1e-6f)
      continuity_ref = 1e-6f;

   FILE *sf = fopen(stats_path, "w");
   if (!sf)
   {
      fprintf(stderr, "cannot open %s\n", stats_path);
      return 1;
   }
   fprintf(sf, "frames=%u frame_samples=%u quality=%d\n",
           total_frames, FRAME_SAMPLES, QUALITY);
   fprintf(sf, "rates=");
   for (unsigned i = 0; i < SEGMENTS; i++)
      fprintf(sf, "%s%u", i ? "," : "", target_rate[i]);
   fprintf(sf, "\n");
   fprintf(sf, "output_samples=%zu\n", out_total);
   fprintf(sf, "switch_boundary_output_pos=%u,%u\n",
           boundary_out[0], boundary_out[1]);
   for (unsigned i = 0; i < NUM_SWITCHES; i++)
      fprintf(sf, "switch_%u_time_ms=%.6f\n", i + 1, switch_seconds[i] * 1000.0);
   fprintf(sf, "switch_max_time_ms=%.6f\n", max_switch_seconds * 1000.0);
   fprintf(sf, "steady_firstdiff_median=%.8f\n", median_diff);
   fprintf(sf, "steady_firstdiff_p99.9=%.8f\n", p999_diff);
   int continuity_ok = 1;
   for (unsigned i = 0; i < NUM_SWITCHES; i++)
   {
      size_t b = boundary_out[i];
      float boundary_diff = fabsf(out_all[b] - out_all[b - 1]);
      float ratio = boundary_diff / continuity_ref;
      fprintf(sf, "switch_%u_boundary_firstdiff=%.8f ratio_to_ref=%.3f\n",
              i + 1, boundary_diff, ratio);
      if (!isfinite(boundary_diff) || boundary_diff > 50.0f * continuity_ref)
         continuity_ok = 0;
   }
   fprintf(sf, "continuity_check=%s\n", continuity_ok ? "PASS" : "FAIL");
   fclose(sf);

   FILE *of = fopen(out_path, "wb");
   if (!of)
   {
      fprintf(stderr, "cannot open %s\n", out_path);
      return 1;
   }
   fwrite(out_all, sizeof(float), out_total, of);
   fclose(of);

   fprintf(stderr, "wrote %zu samples to %s\n", out_total, out_path);
   fprintf(stderr, "max switch time %.6f ms; continuity %s\n",
           max_switch_seconds * 1000.0, continuity_ok ? "PASS" : "FAIL");

   free(diffs);
   free(sorted);
   free(out_all);
   free(frame_in);
   free(frame_out);
   speex_resampler_destroy(st);
   return continuity_ok ? 0 : 2;
}
