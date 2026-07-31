#include "mistercast/groovy_transport.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef MISTERCAST_HAVE_LZ4
extern "C" int LZ4_compress_default(const char*,char*,int,int);
extern "C" int LZ4_compressBound(int);
#endif
namespace mistercast {
static constexpr uint8_t CMD_CLOSE=1,CMD_INIT=2,CMD_SWITCHRES=3,CMD_AUDIO=4,CMD_BLIT_FIELD_VSYNC=7;
GroovyTransport::GroovyTransport()=default;GroovyTransport::~GroovyTransport(){close();}
bool GroovyTransport::sendPacket(const void*p,size_t n,std::string&e){if(fd_<0){e="transport is closed";return false;}auto r=::send(fd_,p,n,MSG_NOSIGNAL);if(r<0||size_t(r)!=n){e=std::string("UDP send failed: ")+std::strerror(errno);return false;}return true;}
bool GroovyTransport::sendChunks(const uint8_t*p,size_t n,std::string&e){while(n){size_t z=std::min<size_t>(mtu_,n);if(!sendPacket(p,z,e))return false;p+=z;n-=z;}return true;}
bool GroovyTransport::open(const std::string&host,uint32_t rate,std::string&e,uint16_t port){close();if(host.empty()){e="target address is required";return false;}addrinfo hint{};hint.ai_family=AF_INET;hint.ai_socktype=SOCK_DGRAM;addrinfo*list=nullptr;auto service=std::to_string(port);int rc=getaddrinfo(host.c_str(),service.c_str(),&hint,&list);if(rc){e=std::string("cannot resolve IPv4 target: ")+gai_strerror(rc);return false;}for(auto*p=list;p;p=p->ai_next){int fd=socket(p->ai_family,p->ai_socktype|SOCK_CLOEXEC,p->ai_protocol);if(fd<0)continue;int snd=2*1024*1024;setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&snd,sizeof(snd));if(connect(fd,p->ai_addr,p->ai_addrlen)==0){fd_=fd;break;}::close(fd);}freeaddrinfo(list);if(fd_<0){e="cannot create UDP connection to target";return false;}uint8_t cmd[5]={CMD_INIT,
#ifdef MISTERCAST_HAVE_LZ4
1,
#else
0,
#endif
uint8_t(rate==22050?1:rate==44100?2:rate==48000?3:0),2,0};if(!cmd[2]){e="unsupported audio sample rate";close();return false;}if(!sendPacket(cmd,sizeof(cmd),e)){close();return false;}pollfd p{fd_,POLLIN,0};if(poll(&p,1,250)<=0){e="target did not acknowledge CMD_INIT (UDP port 32100)";close();return false;}uint8_t ack[32];if(recv(fd_,ack,sizeof(ack),0)<=0){e="invalid CMD_INIT acknowledgment";close();return false;}return true;}
bool GroovyTransport::switchMode(const Modeline&m,std::string&e){if(auto x=m.validate()){e=*x;return false;}uint8_t b[26]{};b[0]=CMD_SWITCHRES;std::memcpy(b+1,&m.pixelClockMHz,8);std::memcpy(b+9,&m.hActive,2);std::memcpy(b+11,&m.hBegin,2);std::memcpy(b+13,&m.hEnd,2);std::memcpy(b+15,&m.hTotal,2);std::memcpy(b+17,&m.vActive,2);std::memcpy(b+19,&m.vBegin,2);std::memcpy(b+21,&m.vEnd,2);std::memcpy(b+23,&m.vTotal,2);b[25]=m.interlaced;frameBytes_=uint32_t(m.hActive)*m.vActive*3/(m.interlaced?2:1);return sendPacket(b,sizeof(b),e);}
bool GroovyTransport::sendFrame(uint32_t frame,uint8_t field,const std::vector<uint8_t>&rgb,std::string&e){if(rgb.size()!=frameBytes_){e="transformed frame has unexpected size";return false;}const uint8_t*payload=rgb.data();size_t bytes=rgb.size();uint32_t csize=0;
#ifdef MISTERCAST_HAVE_LZ4
 compressed_.resize(rgb.size());int z=LZ4_compress_default(reinterpret_cast<const char*>(rgb.data()),reinterpret_cast<char*>(compressed_.data()),int(rgb.size()),int(compressed_.size()));if(z>0){csize=uint32_t(z);bytes=z;payload=compressed_.data();}
#endif
 uint8_t h[12]{};h[0]=CMD_BLIT_FIELD_VSYNC;std::memcpy(h+1,&frame,4);h[5]=field;uint16_t vsync=0;std::memcpy(h+6,&vsync,2);if(csize)std::memcpy(h+8,&csize,4);size_t hs=csize?12:8;if(!sendPacket(h,hs,e))return false;return sendChunks(payload,bytes,e);}
bool GroovyTransport::sendAudio(const int16_t*s,size_t n,std::string&e){size_t bytes=n*sizeof(int16_t);if(bytes>65535){e="audio packet is too large";return false;}uint8_t h[3]={CMD_AUDIO,0,0};uint16_t z=uint16_t(bytes);std::memcpy(h+1,&z,2);return sendPacket(h,3,e)&&sendChunks(reinterpret_cast<const uint8_t*>(s),bytes,e);}
void GroovyTransport::close()noexcept{if(fd_>=0){uint8_t c=CMD_CLOSE;std::string ignored;sendPacket(&c,1,ignored);::close(fd_);fd_=-1;}frameBytes_=0;compressed_.clear();}
}
