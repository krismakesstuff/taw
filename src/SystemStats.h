#pragma once

namespace taw
{
// The machine and taw's share of it, for the stats panel: sample now and then (about once a
// second); CPU figures are the average since the last sample. Negative: not known here.
struct SystemStats
{
    float cpu = -1.0f;    // all cores, 0-1
    float tawCpu = -1.0f; // this process, as a share of all cores, 0-1
    double tawMemoryMB = -1.0;   // this process's private memory
    double freeMemoryMB = -1.0, totalMemoryMB = -1.0;
    int threads = 0;      // logical processors

    void sample();

private:
    unsigned long long lastIdle = 0, lastKernel = 0, lastUser = 0, lastProcess = 0, lastWall = 0;
};
} // namespace taw
