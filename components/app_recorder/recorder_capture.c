#include "recorder_capture.h"

int recorder_capture_push(recorder_session_t *session, const int16_t *pcm, size_t sample_count)
{
    if (!session || (sample_count > 0 && pcm == NULL)) {
        return -1;
    }
    return recorder_session_add_samples(session, RECORDER_CAPTURE_HZ, sample_count);
}
