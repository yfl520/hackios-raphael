/* MIT. Policy only: hardware writes remain in the existing OSM/GMU drivers.
 * Choose solely from firmware-supported points; retain the current maximum.
 */
#ifndef RAPHAEL_POWER_POLICY_H
#define RAPHAEL_POWER_POLICY_H
#include <stdint.h>
#include <stdbool.h>
static inline uint32_t
raphael_cpu_row(const uint32_t khz[40], const bool valid[40], uint32_t low,
    uint32_t high, uint32_t current, uint32_t load, bool boost)
{
    if(boost || low==high || current>=40)return high;
    if(load>100)load=100;
    uint64_t target=(uint64_t)khz[current]*load/65;
    if(target<khz[low])target=khz[low];
    if(target>khz[high])target=khz[high];
    uint32_t best=high;
    for(uint32_t row=0;row<40;row++)
        if(valid[row] && khz[row]>=khz[low] && khz[row]<=khz[high] &&
           khz[row]>=target && khz[row]<khz[best])best=row;
    return best;
}
static inline uint32_t
raphael_gpu_level(uint32_t khz,uint64_t worked_us,uint64_t elapsed_us)
{
    if(!elapsed_us)return 4;
    if(worked_us>elapsed_us)worked_us=elapsed_us;
    /* Product is bounded by 585000 * elapsed_us; caller uses 100ms samples. */
    uint64_t target=(uint64_t)khz*worked_us/elapsed_us*100/65;
    if(target<=257000)return 1;
    if(target<=427000)return 3;
    return 4;
}
#endif
