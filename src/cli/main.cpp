#include "mistercast/config.hpp"
#include "mistercast/interfaces.hpp"
#include "mistercast/stream_session.hpp"
#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>
#ifdef MISTERCAST_HAVE_QT
int launchGui(int,char**);
#endif
using namespace mistercast;
static std::atomic<bool> interrupted{false};static void signalHandler(int){interrupted=true;}
static void usage(){std::cout<<"MiSTerCast for Linux (X11 only)\n\nUsage:\n  mistercast\n  mistercast stream --target HOST [options]\n  mistercast list-monitors\n  mistercast list-modelines\n  mistercast check\n\nStream options: --monitor NAME --modeline 'TIMINGS' --audio --no-audio\n  --crop custom|1x|2x|3x|4x|5x|4:3|5:4 --size WxH --offset X,Y\n  --alignment POSITION --rotation none|cw90|ccw90|180 --frame-delay 0..10 --save\n";}
static bool value(int& i,int n,char**v,std::string&o){if(++i>=n){std::cerr<<"Missing value for "<<v[i-1]<<"\n";return false;}o=v[i];return true;}
int main(int argc,char**argv){if(argc==1){
#ifdef MISTERCAST_HAVE_QT
 return launchGui(argc,argv);
#else
 std::cerr<<"This build has no Qt 6 GUI. Install qt6-base-dev and rebuild, or use 'mistercast --help'.\n";return 2;
#endif
 }std::string command=argv[1];if(command=="--help"||command=="-h"){usage();return 0;}if(command=="list-modelines"){for(auto&m:bundledModelines())std::cout<<m.name<<"\t"<<m.hActive<<"x"<<m.vActive<<(m.interlaced?"i":"p")<<" "<<m.refreshHz()<<" Hz\n";return 0;}if(command=="list-monitors"||command=="check"){auto v=makeX11Capture();std::string e;auto ms=v->monitors(e);if(ms.empty()){std::cerr<<e<<"\n";return 1;}for(auto&m:ms)std::cout<<m.name<<"\t"<<m.width<<"x"<<m.height<<"+"<<m.x<<"+"<<m.y<<(m.primary?" (primary)":"")<<"\n";if(command=="check")std::cout<<"X11 capture: OK\nConfiguration: "<<configPath()<<"\n";return 0;}if(command!="stream"){usage();return 2;}
 AppConfig c=loadConfig(configPath());bool save=false;for(int i=2;i<argc;++i){std::string a=argv[i],x;if(a=="--help"||a=="-h"){usage();return 0;}else if(a=="--target"){if(!value(i,argc,argv,c.target))return 2;}else if(a=="--monitor"){if(!value(i,argc,argv,c.source.monitor))return 2;}else if(a=="--modeline"){if(!value(i,argc,argv,x))return 2;std::string e;if(!parseModeline(x,c.modeline,e)){std::cerr<<e<<"\n";return 2;}}else if(a=="--audio")c.source.audio=true;else if(a=="--no-audio")c.source.audio=false;else if(a=="--crop"){if(!value(i,argc,argv,x)||!parseCropMode(x,c.source.crop)){std::cerr<<"Invalid crop mode\n";return 2;}}else if(a=="--alignment"){if(!value(i,argc,argv,x)||!parseAlignment(x,c.source.alignment)){std::cerr<<"Invalid alignment\n";return 2;}}else if(a=="--rotation"){if(!value(i,argc,argv,x)||!parseRotation(x,c.source.rotation)){std::cerr<<"Invalid rotation\n";return 2;}}else if(a=="--size"){if(!value(i,argc,argv,x)||sscanf(x.c_str(),"%hu%*c%hu",&c.source.width,&c.source.height)!=2){std::cerr<<"Size must be WxH\n";return 2;}}else if(a=="--offset"){if(!value(i,argc,argv,x)||sscanf(x.c_str(),"%hd,%hd",&c.source.xOffset,&c.source.yOffset)!=2){std::cerr<<"Offset must be X,Y\n";return 2;}}else if(a=="--frame-delay"){if(!value(i,argc,argv,x))return 2;try{int n=std::stoi(x);if(n<0||n>10)throw std::out_of_range("x");c.source.frameDelay=n;}catch(...){std::cerr<<"Frame delay must be 0..10\n";return 2;}}else if(a=="--save")save=true;else{std::cerr<<"Unknown option: "<<a<<"\n";return 2;}}
 if(c.target.empty()){std::cerr<<"A target is required; use --target HOST.\n";return 2;}if(save){if(c.modeline.name=="Custom")c.customModelines.push_back(c.modeline);std::string e;if(!saveConfig(c,configPath(),e)){std::cerr<<"Cannot save settings: "<<e<<"\n";return 1;}}
 std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);StreamSession session;std::string e;if(!session.start(c,[](SessionState s,const std::optional<SessionError>&x){if(x)std::cerr<<x->component<<": "<<x->message<<"\n";else if(s==SessionState::Streaming)std::cerr<<"Streaming; press Ctrl-C to stop.\n";},&e)){std::cerr<<"Start failed: "<<e<<"\n";return 1;}auto nextStats=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!interrupted&&session.state()==SessionState::Streaming){std::this_thread::sleep_for(std::chrono::milliseconds(100));if(std::chrono::steady_clock::now()>=nextStats){auto s=session.stats();std::cerr<<"video "<<s.streamFps<<" fps, capture "<<s.captureFps<<" fps, dropped "<<s.droppedFrames<<", audio buffered "<<s.audioBufferedSamples<<" samples, overruns "<<s.audioDroppedSamples<<", underruns "<<s.audioUnderrunSamples<<"\n";nextStats+=std::chrono::seconds(5);}}bool failed=session.state()==SessionState::Error;session.stop();return failed?1:0;}
