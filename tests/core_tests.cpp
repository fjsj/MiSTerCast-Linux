#include "mistercast/audio_ring.hpp"
#include "mistercast/audio_pacer.hpp"
#include "mistercast/config.hpp"
#include "mistercast/groovy_transport.hpp"
#include "mistercast/transform.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace mistercast;
static int failed=0;
#define CHECK(x) do{if(!(x)){std::cerr<<__FILE__<<":"<<__LINE__<<": CHECK failed: "#x"\n";++failed;}}while(0)
int main(){
 auto safe=Modeline::safeDefault();CHECK(!safe.validate());CHECK(safe.refreshHz()>59&&safe.refreshHz()<61);auto bad=safe;bad.hTotal=300;CHECK(bool(bad.validate()));
 Modeline parsed;std::string error;CHECK(parseModeline("6.7 320 336 367 426 240 244 247 262 0",parsed,error));CHECK(!parseModeline("nonsense",parsed,error));
 AudioRing ring(4);int16_t a[]={1,2,3};CHECK(ring.push(a,3)==0);int16_t o[4];CHECK(ring.pop(o,4)==3);CHECK(o[0]==1&&o[2]==3&&o[3]==0);int16_t b[]={1,2,3,4,5};CHECK(ring.push(b,5)==1);CHECK(ring.pop(o,4)==4&&o[0]==2&&o[3]==5);
 AudioPacer pacer(48000);uint64_t pacedValues=0,totalNs=0;for(int i=0;i<1000;++i){uint64_t elapsed=16682885+(i%3);totalNs+=elapsed;auto due=pacer.valuesDue(elapsed,32000);CHECK((due&1)==0);pacedValues+=due;}CHECK(pacedValues==(totalNs*48000/1000000000)*2);pacer.reset(48000);CHECK(pacer.valuesDue(1000000000,32000)==32000);CHECK(pacer.valuesDue(0,100000)==64000);
 Frame f;f.width=4;f.height=2;f.stride=16;f.bgra.resize(32);for(size_t i=0;i<8;++i){f.bgra[i*4]=uint8_t(i);f.bgra[i*4+1]=10;f.bgra[i*4+2]=20;f.bgra[i*4+3]=255;}SourceOptions so;so.crop=CropMode::Custom;so.width=4;so.height=2;Modeline m{"test",1,2,3,4,5,2,3,4,5,false};std::vector<uint8_t> rgb;CHECK(transformRgb24(f,so,m,0,rgb,error));CHECK(rgb.size()==12&&rgb[0]==1&&rgb[9]==7);m.interlaced=true;CHECK(transformRgb24(f,so,m,0,rgb,error));CHECK(rgb.size()==6&&rgb[0]==5);CHECK(transformRgb24(f,so,m,1,rgb,error));CHECK(rgb.size()==6&&rgb[0]==1);
 uint8_t px[]={0x00,0x00,0xff,0x00};Frame nf;CHECK(normalizeToBgra(px,4,1,1,4,32,0xff0000,0xff00,0xff,true,nf,error));CHECK(nf.bgra[0]==0&&nf.bgra[1]==0&&nf.bgra[2]==255);
 int server=socket(AF_INET,SOCK_DGRAM,0);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);if(server>=0&&bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0){socklen_t alen=sizeof(address);getsockname(server,reinterpret_cast<sockaddr*>(&address),&alen);timeval timeout{2,0};setsockopt(server,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));std::atomic<bool>sawMode=false,sawFrame=false,sawAudio=false,sawClose=false;std::atomic<uint16_t>receivedSyncLine=0;std::thread endpoint([&]{uint8_t packet[2048];sockaddr_storage peer{};socklen_t plen=sizeof(peer);for(int i=0;i<12&&!sawClose;++i){auto n=recvfrom(server,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&peer),&plen);if(n<=0)break;if(packet[0]==2){uint8_t ack[13]{};sendto(server,ack,sizeof(ack),0,reinterpret_cast<sockaddr*>(&peer),plen);}else if(packet[0]==3)sawMode=true;else if(packet[0]==7){sawFrame=true;uint32_t frame;uint16_t line;std::memcpy(&frame,packet+1,4);std::memcpy(&line,packet+6,2);receivedSyncLine=line;uint8_t ack[13]{};std::memcpy(ack,&frame,4);std::memcpy(ack+4,&line,2);uint32_t fpgaFrame=frame+1;uint16_t fpgaLine=1;std::memcpy(ack+6,&fpgaFrame,4);std::memcpy(ack+10,&fpgaLine,2);ack[12]=0x44;sendto(server,ack,sizeof(ack),0,reinterpret_cast<sockaddr*>(&peer),plen);}else if(packet[0]==4)sawAudio=true;else if(packet[0]==1)sawClose=true;}});{GroovyTransport transport;CHECK(transport.open("localhost",48000,error,ntohs(address.sin_port)));Modeline tiny{"tiny",1,2,3,4,5,2,3,4,5,false};CHECK(transport.switchMode(tiny,error));transport.setSyncOptions(true,0);std::vector<uint8_t> pixels(12,42);CHECK(transport.sendFrame(1,0,pixels,error));transport.waitSync();auto status=transport.stats();CHECK(status.acknowledgedFrame==1&&status.acknowledgedFrames==1&&status.rasterCorrectionUs<0&&status.vramSynced&&transport.misterAudioEnabled());int16_t sound[4]{};CHECK(transport.sendAudio(sound,4,error));transport.close();}endpoint.join();CHECK(sawMode&&sawFrame&&sawAudio&&sawClose);CHECK(receivedSyncLine==2);}if(server>=0)close(server);
 auto dir=std::filesystem::temp_directory_path()/"mistercast-core-test";std::filesystem::create_directories(dir);auto path=dir/"config.json";AppConfig cfg;cfg.target="mister.local";auto custom=Modeline::safeDefault();custom.name="My preset";cfg.customModelines.push_back(custom);CHECK(saveConfig(cfg,path,error));std::string warning;auto loaded=loadConfig(path,&warning);CHECK(loaded.target==cfg.target&&warning.empty());CHECK(loaded.customModelines.size()==1&&loaded.customModelines[0].name=="My preset");{std::ofstream f2(path);f2<<"broken";}loaded=loadConfig(path,&warning);CHECK(loaded.target.empty()&&!warning.empty());std::filesystem::remove_all(dir);
 if(failed)std::cerr<<failed<<" test(s) failed\n";return failed?1:0;
}
