#pragma once
#include <Arduino.h>
#include <atomic>
#include <math.h>
#include "RadioVizMotion.h"

// Eight frequency probes (Goertzel), not random animation. Decoder owns the
// analysis state; the main loop reads atomic levels. No PCM copy, heap, or FFT
// workspace. At most one bounded 512-frame analysis every 50 ms.
static std::atomic<bool> radioVizEnabled{false};
static std::atomic<uint8_t> radioVizLevels[8];
static std::atomic<uint32_t> radioVizUpdated{0};
static std::atomic<uint16_t> radioVizRmsQ15{0};
static std::atomic<uint32_t> radioVizMaxAnalysisUs{0};
static void radioVizAnalyze(const int16_t* pcm,size_t count,int channels,int rate) {
  if(!radioVizEnabled.load(std::memory_order_relaxed) || channels<1 || channels>2 || rate<=0)return;
  static uint32_t last=0;
  const uint32_t now=millis();
  if(now-last<50 || count/size_t(channels)<512)return;
  last=now;
  const uint32_t started=micros();
  const float frequencies[8]={180,350,700,1200,2200,3600,5500,8500};
  float coefficient[8],a[8]={},b[8]={};
  for(int k=0;k<8;++k)coefficient[k]=2*cosf(6.2831853f*frequencies[k]/rate);
  float energy=0;
  for(int n=0;n<512;++n) {
    const float left=pcm[n*channels]/32768.0f;
    const float right=channels==2?pcm[n*2+1]/32768.0f:left;
    energy+=(left*left+right*right)*0.5f;
    float sample=pcm[n*channels];
    if(channels==2)sample=(sample+pcm[n*2+1])*0.5f;
    // Triangular window reduces leakage, with no per-sample trig calls.
    sample*= (1.0f-fabsf((n-255.5f)/256.0f))/32768.0f;
    for(int k=0;k<8;++k) {
      float next=sample+coefficient[k]*a[k]-b[k];
      b[k]=a[k];a[k]=next;
    }
  }
  for(int k=0;k<8;++k) {
    float power=a[k]*a[k]+b[k]*b[k]-coefficient[k]*a[k]*b[k];
    float db=10*log10f(fmaxf(power/(128.0f*128.0f),1e-10f));
    float level=fmaxf(0,fminf(12,(db+65)*12/55));
    radioVizLevels[k].store(frequencies[k]<rate*0.48f?uint8_t(level*16):0,std::memory_order_relaxed);
  }
  radioVizRmsQ15.store(uint16_t(fminf(32767,sqrtf(float(energy/512))*32768)),std::memory_order_relaxed);
  radioVizUpdated.store(now,std::memory_order_release);
  uint32_t elapsed=micros()-started;
  if(elapsed>radioVizMaxAnalysisUs.load(std::memory_order_relaxed))
    radioVizMaxAnalysisUs.store(elapsed,std::memory_order_relaxed);
}
