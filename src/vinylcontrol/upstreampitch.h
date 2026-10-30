#pragma once
#include <stddef.h>

struct QuietVinylUpstreamPitch;
struct QuietVinylUpstreamPitch* qv_pitch_create(const char* name, double speed, unsigned sample_rate);
void qv_pitch_submit(struct QuietVinylUpstreamPitch* pitch, short* pcm, size_t frames);
double qv_pitch_rate(struct QuietVinylUpstreamPitch* pitch);
void qv_pitch_destroy(struct QuietVinylUpstreamPitch* pitch);
void qv_pitch_free_lookups(void);
