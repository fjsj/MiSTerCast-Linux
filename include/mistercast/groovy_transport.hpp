#pragma once
#include "mistercast/types.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
namespace mistercast {
class GroovyTransport {
public:
 GroovyTransport(); ~GroovyTransport(); GroovyTransport(const GroovyTransport&)=delete;GroovyTransport&operator=(const GroovyTransport&)=delete;
 bool open(const std::string& host,uint32_t audioRate,std::string& error,uint16_t port=32100);
 bool switchMode(const Modeline&,std::string& error);
 bool sendFrame(uint32_t frame,uint8_t field,const std::vector<uint8_t>&rgb,std::string&error);
 bool sendAudio(const int16_t*samples,size_t count,std::string&error);
 void close()noexcept; bool connected()const noexcept{return fd_>=0;}
 bool misterAudioEnabled()const noexcept{return misterAudioEnabled_.load();}
private:int fd_{-1};uint32_t frameBytes_{};uint16_t mtu_{1472};std::vector<uint8_t> compressed_;bool sendPacket(const void*,size_t,std::string&);bool sendChunks(const uint8_t*,size_t,std::string&);
 std::atomic<bool>misterAudioEnabled_{false};void drainStatus()noexcept;
};
}
