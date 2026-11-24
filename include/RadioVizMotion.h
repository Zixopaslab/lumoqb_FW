#pragma once
#include <stdint.h>

// Q4 heights: 16 brightness steps per segment, 12 segments. Faster attack
// follows beats; slower release avoids jitter. Work is bounded after a pause.
#if __cplusplus >= 201402L
constexpr
#endif
inline uint8_t radioVizEase(uint8_t previous,uint8_t target,uint32_t elapsed) {
  if(target>192)target=192;
  if(previous>192)previous=192;
  if(elapsed>100)elapsed=100;
  if(!elapsed || previous==target)return previous;
  bool rising=target>previous;
  unsigned delta=rising?target-previous:previous-target;
  unsigned tau=rising?70:220;
  unsigned step=(delta*elapsed+(tau+elapsed)/2)/(tau+elapsed);
  if(!step)step=1;
  return rising?previous+step:previous-step;
}
inline uint8_t radioVizBrightness(uint8_t height,int segment) {
  int remaining=int(height)-segment*16;
  return remaining<0?0:remaining>16?16:remaining;
}
