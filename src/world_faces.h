#pragma once

// Only selected entries live on the ESP32. Directories and search live on the phone.
struct WorldClock { char name[25]; char tz[65]; };
struct WorldWeather { char name[25]; char country[3]; float lat,lon; };
struct WorldStock { char symbol[13]; char exchange[25]; char currency[4]; };
struct WorldRadio { char name[33]; char host[97]; char path[193]; uint16_t port; };
static WorldClock worldClocks[6];
static WorldWeather worldWeather[6];
static WorldStock worldStocks[6];
static WorldRadio worldRadio[6];
static uint8_t worldEnabled[4]={};
static int worldApp(const char* app) {
  const char* names[]={"clock","weather","stock","radio"};
  for(int i=0;i<4;i++)if(!strcmp(app,names[i]))return i;
  return -1;
}
static bool worldText(JsonVariantConst value,size_t max,bool empty=false) {
  if(!value.is<const char*>())return false;
  const char* s=value.as<const char*>();size_t n=strlen(s);
  if(n>max || (!empty&&!n))return false;
  for(size_t i=0;i<n;i++)if(s[i]<32 || s[i]>126)return false;
  return true;
}
// Validate before persistence or modifying live pointers. JSON is bounded to 511 bytes.
static bool worldRecord(int app,int side,JsonDocument& doc,bool apply) {
  if(app<0||app>3||side<0||side>5)return false;
  if(doc["preset"].is<bool>() && doc["preset"].as<bool>()) {
    if(apply)worldEnabled[app]&=~(1<<side);
    return true;
  }
  if(app==0) {
    if(!worldText(doc["name"],24)||!worldText(doc["tz"],64))return false;
    const char* tz=doc["tz"];if(strspn(tz,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789,+-:./<>")!=strlen(tz))return false;
    if(apply){strlcpy(worldClocks[side].name,doc["name"],25);strlcpy(worldClocks[side].tz,tz,65);}
  } else if(app==1) {
    if(!worldText(doc["name"],24)||!worldText(doc["country"],2)||!doc["lat"].is<float>()||!doc["lon"].is<float>())return false;
    float lat=doc["lat"],lon=doc["lon"];if(!isfinite(lat)||!isfinite(lon)||lat< -90||lat>90||lon< -180||lon>180)return false;
    if(apply){auto& v=worldWeather[side];strlcpy(v.name,doc["name"],25);strlcpy(v.country,doc["country"],3);v.lat=lat;v.lon=lon;}
  } else if(app==2) {
    if(!worldText(doc["symbol"],12)||!validTicker(doc["symbol"])||!worldText(doc["exchange"],24,true)||!worldText(doc["currency"],3,true))return false;
    if(apply){strlcpy(worldStocks[side].symbol,doc["symbol"],13);strlcpy(worldStocks[side].exchange,doc["exchange"],25);strlcpy(worldStocks[side].currency,doc["currency"],4);}
  } else {
    if(!worldText(doc["name"],32)||!worldText(doc["host"],96)||!worldText(doc["path"],192)||!doc["port"].is<int>())return false;
    const char* host=doc["host"];const char* path=doc["path"];int port=doc["port"];
    if(port<1||port>65535||path[0]!='/'||strchr(path,' ')||strchr(path,'#')||strspn(host,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-")!=strlen(host))return false;
    if(apply){auto& v=worldRadio[side];strlcpy(v.name,doc["name"],33);strlcpy(v.host,host,97);strlcpy(v.path,path,193);v.port=port;}
  }
  if(apply)worldEnabled[app]|=1<<side;
  return true;
}
static void applyWorldFaces() {
  for(int i=0;i<6;i++) {
    if(worldEnabled[0]&(1<<i)){CLOCK_REGIONS[i].cityName=worldClocks[i].name;CLOCK_REGIONS[i].posixTimezone=worldClocks[i].tz;}
    if(worldEnabled[1]&(1<<i)){auto& v=worldWeather[i];WEATHER_CITIES[i]={v.name,v.country,v.lat,v.lon};}
    if(worldEnabled[2]&(1<<i))STOCKS[i]={worldStocks[i].symbol,worldStocks[i].symbol};
    if(worldEnabled[3]&(1<<i)){auto& v=worldRadio[i];RADIO_STATIONS[i]={v.name,"",v.host,v.path,v.port};}
  }
}
static void loadWorldFaces() {
  Preferences storage;if(!storage.begin("world_faces",true))return;
  StaticJsonDocument<1024> doc;
  for(int a=0;a<4;a++)for(int side=0;side<6;side++) {
    char key[8];snprintf(key,sizeof(key),"%d_%d",a,side);
    if(!storage.isKey(key))continue;
    String json=storage.getString(key,"");if(json.length()>511)continue;
    doc.clear();if(!deserializeJson(doc,json))worldRecord(a,side,doc,true);
  }
  storage.end();applyWorldFaces();
}
static portMUX_TYPE worldMux=portMUX_INITIALIZER_UNLOCKED;
static char worldCommand[183]={};
static bool worldPending=false;
static bool queueWorldCommand(const std::string& json) {
  if(json.size()>182)return false;
  portENTER_CRITICAL(&worldMux);
  if(worldPending){portEXIT_CRITICAL(&worldMux);return false;}
  memcpy(worldCommand,json.c_str(),json.size()+1);worldPending=true;
  portEXIT_CRITICAL(&worldMux);return true;
}
// One transaction at a time; partial/disconnected transfers never change saved settings.
static char worldUpload[512]={};
static uint32_t worldTransaction=0,worldUploadAt=0;
static int worldUploadApp=-1,worldUploadSide=-1,worldExpected=0,worldReceived=0;
static int unhex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static String worldEscape(const char* text) {
  String out;const char* hex="0123456789ABCDEF";
  for(const uint8_t* p=(const uint8_t*)text;*p;p++) {
    if(isalnum(*p)||*p=='-'||*p=='.'||*p=='_')out+=(char)*p;
    else {out+='%';out+=hex[*p>>4];out+=hex[*p&15];}
  }
  return out;
}
