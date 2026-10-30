#include "upstreampitch.h"
#include "timecoder.h"
#include <stdlib.h>

struct QuietVinylUpstreamPitch { struct timecoder decoder; };

struct QuietVinylUpstreamPitch* qv_pitch_create(const char* name, double speed, unsigned sample_rate) {
    struct timecode_def* def = timecoder_find_definition(name, NULL);
    if (!def) return NULL;
    struct QuietVinylUpstreamPitch* pitch = calloc(1, sizeof(*pitch));
    if (pitch) timecoder_init(&pitch->decoder, def, speed, sample_rate, false, false);
    return pitch;
}
void qv_pitch_submit(struct QuietVinylUpstreamPitch* pitch, short* pcm, size_t frames) {
    if (pitch) timecoder_submit(&pitch->decoder, pcm, frames);
}
double qv_pitch_rate(struct QuietVinylUpstreamPitch* pitch) {
    return pitch ? timecoder_get_pitch(&pitch->decoder) : 0;
}
void qv_pitch_destroy(struct QuietVinylUpstreamPitch* pitch) {
    if (pitch) timecoder_clear(&pitch->decoder);
    free(pitch);
}
void qv_pitch_free_lookups(void) { timecoder_free_lookup(); }
