#ifndef CSDUP_PACING_H
#define CSDUP_PACING_H

/* When the next frame may be handed over: one interval after the last due time, so small
 * delays do not lower the average rate, but never more than one interval after now. */
static long long next_frame_due(long long due, long long now, long long interval)
{
    due += interval;
    return due < now ? now + interval : due;
}

#endif
