#pragma once
// Radio owns I2S playback. No microphone, recognition or cloud assistant calls.
static bool ferroRadioActive=false;
static uint32_t ferroRadioPollAt=0,ferroEnvelopeAt=0,ferroAudioStatsAt=0;
static float ferroEnvelope=0,ferroLevelAverage=0;
static float ferroTint[3]={};
static float ferroColorPhase=0,ferroColorPreviousLevel=0;
static uint8_t ferroPalette[16][3],ferroBandPalette[3][16][3];
static float ferroBandLevel[3]={};
static int16_t ferroPatchX[3]={},ferroPatchY[3]={};
static uint16_t ferroPatchScaleX[3]={},ferroPatchScaleY[3]={};
static uint8_t ferroColorStyle(){return musicModeActive?musicFerroStyle:radioFerroStyle;}
static void ferroUpdateBands(bool fresh,uint32_t elapsed){
  // Three broad, feathered pools of color follow bass, mids and treble.
  // Offset centers avoid a stack of equalizer bars or dots.
  static const uint8_t colors[3][3]={{255,45,8},{5,255,205},{165,12,255}};
  static const uint8_t first[3]={0,3,6},last[3]={3,6,8};
  for(int band=0;band<3;band++){
    float target=0;
    if(fresh)for(int probe=first[band];probe<last[band];probe++)
      target+=radioVizLevels[probe].load(std::memory_order_relaxed)/192.0f;
    target/=last[band]-first[band];
    const float ease=1-expf(-float(elapsed)/(target>ferroBandLevel[band]?35.0f:160.0f));
    ferroBandLevel[band]+=(target-ferroBandLevel[band])*ease;
    const float level=constrain(ferroBandLevel[band],0.0f,1.0f);
    ferroPatchX[band]=band==0?58+int(8*level):band==1?88-int(6*level):66+int(5*level);
    ferroPatchY[band]=band==0?100-int(8*level):band==1?73:45+int(5*level);
    const int radiusX=23+int(18*level),radiusY=24+int(12*level);
    ferroPatchScaleX[band]=65536/(radiusX*radiusX);
    ferroPatchScaleY[band]=65536/(radiusY*radiusY);
    for(int shade=0;shade<16;shade++)for(int channel=0;channel<3;channel++){
      unsigned gray=shade*17,tint=unsigned(colors[band][channel]*min(1.0f,level*4.0f));
      ferroBandPalette[band][shade][channel]=gray+(255-gray)*tint/255;
    }
  }
}

static void ferroUpdateColor(bool fresh,uint32_t elapsed){
  // Saturated rainbow: never average unrelated bands into a muted color.
  // Music energy advances the palette; rising beats give it larger steps.
  static const uint8_t colors[7][3]={
    {225,8,12},{225,210,8},{8,215,20},{8,210,225},
    {15,25,235},{225,8,215},{225,8,12}
  };
  const float rise=max(0.0f,ferroEnvelope-ferroColorPreviousLevel);
  ferroColorPreviousLevel=ferroEnvelope;
  if(fresh&&ferroEnvelope>0.035f){
    ferroColorPhase+=rise*1.2f+ferroEnvelope*float(elapsed)/5000.0f;
    ferroColorPhase-=floorf(ferroColorPhase);
  }
  const float position=ferroColorPhase*6.0f;
  const int segment=min(5,int(position));const float blend=position-segment;
  const float strength=fresh?min(1.0f,ferroEnvelope*8.0f):0;
  const float ease=1-expf(-float(elapsed)/80.0f);
  for(int channel=0;channel<3;channel++){
    const float target=(colors[segment][channel]*(1-blend)+colors[segment+1][channel]*blend)*strength;
    ferroTint[channel]+=(target-ferroTint[channel])*ease;
    const unsigned tint=unsigned(constrain(ferroTint[channel],0.0f,255.0f));
    for(int shade=0;shade<16;shade++){
      const unsigned gray=shade*17;
      // Preserve white background, antialiasing and ordered depth shading.
      ferroPalette[shade][channel]=gray+(255-gray)*tint/255;
    }
  }
}
static void ferroRadioReset(){
  if(ferroRadioActive){stopRadioStream();radioSelectedStation=-1;}
  ferroRadioActive=false;ferroEnvelope=0;ferroLevelAverage=0;ferroEnvelopeAt=0;
  for(int channel=0;channel<3;channel++)ferroTint[channel]=0;
  ferroColorPhase=ferroColorPreviousLevel=0;
  for(int band=0;band<3;band++)ferroBandLevel[band]=0;
}
static void ferroRadioBegin(){
  if(musicModeActive){ferroRadioActive=false;ferroEnvelope=0;ferroLevelAverage=0;ferroEnvelopeAt=0;return;}
  ferroRadioActive=true;ferroRadioPollAt=0;ferroAudioStatsAt=millis();
  radioSelectedStation=-1;radioLastOrientation=0;radioCandidateOrientation=0;
  radioCandidateSinceMs=0;radioLastOrientationPollMs=0;
  Serial.println("Ferrofluid: radio playback only; turn cube to choose station; microphone and voice commands disabled.");
}
static void ferroAudioUpdate(){
  if(!ferroRadioActive||ferroFace!=0||millis()-ferroRadioPollAt<80)return;
  ferroRadioPollAt=millis();
  // Reuse station selection, Wi-Fi connection and bounded reconnect policy.
  const bool oldFast=getFastPeripheralSelection();
  updateRadioMode();setFastPeripheralSelection(oldFast);

}
static uint16_t ferroAudioFrame(uint32_t now){
  const uint32_t updated=radioVizUpdated.load(std::memory_order_acquire);
  now=millis(); // Sample after connection work; old timestamps underflow against fresh PCM.
  const bool fresh=(musicModeActive?musicAudioPlaying.load():mp3TaskHandle!=nullptr)&&radioVizEnabled.load()&&updated&&now-updated<500;
  const float rms=fresh?radioVizRmsQ15.load(std::memory_order_relaxed)/32768.0f:0;
  // More motion in quiet passages, with headroom for sudden musical accents.
  const float db=20*log10f(max(rms,0.00001f));
  const float normalized=constrain((db+60.0f)/42.0f,0.0f,1.0f);
  const uint32_t elapsed=ferroEnvelopeAt?min(uint32_t(1000),now-ferroEnvelopeAt):50;
  ferroEnvelopeAt=now;
  const float accent=fresh?min(0.25f,max(0.0f,normalized-ferroLevelAverage)*0.8f):0;
  ferroLevelAverage+=(normalized-ferroLevelAverage)*(1-expf(-float(elapsed)/600.0f));
  const float target=min(1.0f,0.8f*powf(normalized,1.4f)+accent);
  // Fast attack catches beats; quicker release separates successive beats.
  const float response=1-expf(-float(elapsed)/(target>ferroEnvelope?25.0f:130.0f));
  ferroEnvelope+=(target-ferroEnvelope)*response;
  if(ferroColorStyle()==2)ferroUpdateColor(fresh,elapsed);
  else if(ferroColorStyle()==3)ferroUpdateBands(fresh,elapsed);
  if(now-ferroAudioStatsAt>=5000){
    ferroAudioStatsAt=now;
    Serial.printf("Ferrofluid radio: RMS %.3f, response %.2f, zoom %.2f, fresh %d.\n",rms,ferroEnvelope,1+.55f*ferroEnvelope,int(fresh));
  }
  return ferroEnvelope>.035f?60+min(16,int(ferroEnvelope*16)):uint16_t((now/125)%13);
}
static uint16_t ferroVisualZoom(uint32_t){return 1000+uint16_t(550*ferroEnvelope);}
