#include <assert.h>
#include <stdio.h>
#include "../shim/pacing.h"

int main(void)
{
    /* A frame handed over 6 ms late does not push the schedule back. */
    assert(next_frame_due(100, 106, 50) == 150);
    /* Steady late handovers still average one frame per interval. */
    long long due = 0;
    for (int i = 0; i < 20; ++i) due = next_frame_due(due, due + 7, 50);
    assert(due == 1000);
    /* After a still screen, the schedule restarts from now instead of releasing a burst. */
    assert(next_frame_due(100, 1000, 50) == 1050);
    puts("pacing ok");
    return 0;
}
