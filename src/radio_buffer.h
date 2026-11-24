#pragma once
#include <WiFiClient.h>
#include <lwip/sockets.h>
#include <errno.h>

// Single-owner reservoir, serviced by the decoder task. No extra RTOS task or
// cross-core buffer locks. Release only after the decoder has stopped.
class RadioBufferedStream : public Stream {
  WiFiClient* client=nullptr;
  uint8_t* data=nullptr;
  // The still-image JPEG decoder is now on-demand, freeing 17,888 bytes.
  // Spend 6 KB of that saving on jitter buffering while retaining TCP headroom.
  static constexpr size_t capacity=8192, prefill=4096;
  size_t head=0, count=0;
  bool started=false;
  uint32_t starvations=0;
  void fill() {
    while(data && count<capacity && client->available()>0 && !audio_stop_requested) {
      size_t tail=(head+count)%capacity;
      size_t n=min(capacity-count,capacity-tail);
      n=min(n,size_t(client->available()));
      int received=client->read(data+tail,n);
      if(received<=0)break;
      count+=received;
    }
  }
public:
  bool begin(WiFiClient& source) {
    end();data=static_cast<uint8_t*>(malloc(capacity));
    client=&source;return data!=nullptr;
  }
  void end() { free(data);data=nullptr;client=nullptr;head=count=0;started=false;starvations=0; }
  size_t write(uint8_t) override { return 0; }
  int available() override { if(client)fill();return count; }
  int peek() override { return available()?data[head]:-1; }
  int read() override { uint8_t b;return readBytes(&b,1)==1?b:-1; }
  void flush() override {}
  using Stream::readBytes;
  size_t readBytes(char* out,size_t length) override {
    if(!data || !client || !length || audio_stop_requested)return 0;
    fill();
    if(!started || count==0) {
      const bool recovering=started;
      // Once playing, hand the first arriving bytes straight to Helix. Waiting
      // for another full prefill adds avoidable silence to each network gap.
      const size_t target=recovering ? 1 : prefill;
      uint32_t began=millis();
      while(count<target && !audio_stop_requested && millis()-began<5000) {
        fill();
        if(!client->connected() && client->available()==0)break;
        if(count<target)vTaskDelay(1);
      }
      if(audio_stop_requested)return 0;
      if (!count) {
        uint8_t probe;
        errno=0;
        const int peekResult=recv(client->fd(), &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        const int socketError=errno;
        Serial.printf("Radio: empty input socket peek=%d errno=%d (0=peer closed).\n",
                      peekResult, socketError);
      }
      if(recovering)Serial.printf("Radio: network buffer underrun #%u; waited %u ms, buffered %u bytes, connected %u, heap %u, largest %u.\n",
                                  ++starvations,unsigned(millis()-began),unsigned(count),
                                  unsigned(client->connected()), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      else Serial.printf("Radio: prebuffered %u bytes.\n",unsigned(count));
      started=true;
    }
    // Do not wait for a complete MP3_MAX_FRAME_SIZE when some data is ready.
    size_t n=min(length,count), first=min(n,capacity-head);
    memcpy(out,data+head,first);memcpy(out+first,data,n-first);
    head=(head+n)%capacity;count-=n;return n;
  }
};
