#include "rooms_native.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <set>
#include <cctype>
#ifdef __ANDROID__
#include <android/log.h>
#endif
namespace mkdd::native_rooms {
namespace {
constexpr const char* alphabet="23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
Json arr(){Json j;j.kind=Json::Array;return j;}
Json defaults(const std::string& mode){return mode=="grand-prix"?Json::dict({{"engine","150cc"},{"cup","mushroom"}}):Json::dict({{"arena",52},{"rounds",3}});}
}
std::string Rooms::random_text(size_t count,const std::string& chars){
 std::ifstream random("/dev/urandom",std::ios::binary);
 if(!random)throw std::runtime_error("secure-random-unavailable");
 std::string out;out.reserve(count);
 const unsigned bound=256-(256%chars.size());
 while(out.size()<count){unsigned char byte=0;random.read(reinterpret_cast<char*>(&byte),1);if(!random)throw std::runtime_error("secure-random-failed");if(byte<bound)out+=chars[byte%chars.size()];}
 return out;
}
std::string Rooms::required(const Json& obj,const char* key,size_t limit){
 if(!obj.has(key)||obj.at(key).kind!=Json::String)throw std::runtime_error("invalid-text");
 std::string s=obj.at(key).str();
 if(s.empty()||s.size()>limit||std::any_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;}))throw std::runtime_error("invalid-text");
 auto a=s.find_first_not_of(" \t");if(a==std::string::npos)throw std::runtime_error("invalid-text");auto b=s.find_last_not_of(" \t");return s.substr(a,b-a+1);
}
std::string Rooms::optional(const Json& obj,const char* key,const char* fallback){return obj.has(key)?obj.at(key).str():fallback;}
Json Rooms::snapshot(const Room& r,const std::string& token){
 Json members=arr();
 std::set<unsigned> karts;
 for(const auto& [t,m]:r.members){
  karts.insert(m.seat<8?m.seat:m.seat-8);
  const char* role=r.layout=="solo"?"solo":m.seat>=8?"passenger":"driver";
  Json characters=arr();for(int id:m.characters)characters.array.emplace_back(id);
  const auto choice=Json::dict({{"kart",m.kart},{"driver",m.driver},{"partner",m.partner}});
  members.array.push_back(Json::dict({{"id",m.uid},{"name",m.name},{"initials",m.initials},
      {"seat",m.seat},{"ready",m.ready},{"you",t==token},{"host",t==r.host},
      {"role",role},{"stars",0},{"kart",m.seat<8?m.seat:m.seat-8},
      {"characters",characters},{"selected",m.selected},{"kart_selected",m.kart_selected},{"choice",choice}}));
 }
 const unsigned human_karts=std::max(1u,(unsigned)karts.size());
 const unsigned cpu_karts=r.mode=="grand-prix"?8-human_karts:0;
 return Json::dict({{"code",r.code},{"revision",r.revision},{"state",r.state},{"mode",r.mode},{"layout",r.layout},{"flow",r.flow},{"rules",r.rules},{"capacity",8},{"minimum_players",2},{"human_karts",(int64_t)human_karts},{"cpu_karts",(int64_t)cpu_karts},{"host",token==r.host},{"members",members}});
}
Json Rooms::request(const Json& req){
 std::lock_guard<std::mutex> guard(mutex_);
 std::string op_for_log="?",state_for_log="?";
 try {
  if(req.kind!=Json::Object)throw std::runtime_error("invalid-request");
  const std::string op=required(req,"op",32);op_for_log=op;
  if(op=="create"){
   if(rooms_.size()>=256)throw std::runtime_error("service-full");
   std::string name=required(req,"name",16),compat=required(req,"compatibility",128);
   std::string mode=optional(req,"mode","grand-prix"),layout=optional(req,"layout","solo"),flow=optional(req,"flow","legacy");
   if(std::set<std::string>{"grand-prix","balloon","shine","bob-omb"}.count(mode)==0||std::set<std::string>{"solo","coop","mixed"}.count(layout)==0||std::set<std::string>{"legacy","staged","garage"}.count(flow)==0||(flow=="garage"?(layout!="solo"&&layout!="mixed"):layout=="mixed"))throw std::runtime_error("invalid-rules");
   Room r;r.compatibility=compat;r.mode=mode;r.layout=layout;r.flow=flow;r.rules=defaults(mode);
   if(req.has("rules")){if(req.at("rules").kind!=Json::Object)throw std::runtime_error("invalid-rules");for(const auto& [k,v]:req.at("rules").object)r.rules.object[k]=v;}
   do{r.code=random_text(8,alphabet);}while(rooms_.count(r.code));
   r.host=random_text(43,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
   Member m;m.token=r.host;m.name=name;m.initials=optional(req,"initials","P1");m.uid=random_text(16,"0123456789abcdef");
   r.members.emplace(m.token,m);tokens_[m.token]=r.code;
   Json response=Json::dict({{"token",r.host},{"room",snapshot(r,r.host)}});
   rooms_.emplace(r.code,std::move(r));return response;
  }
  if(op=="join"){
   std::string code=required(req,"code",16);code.erase(std::remove_if(code.begin(),code.end(),[](unsigned char c){return c=='-'||c==' ';}),code.end());
   std::transform(code.begin(),code.end(),code.begin(),[](unsigned char c){return std::toupper(c);});
   auto it=rooms_.find(code);if(it==rooms_.end())throw std::runtime_error("room-not-found");
   Room& r=it->second;
   if(!req.has("compatibility")||req.at("compatibility").str()!=r.compatibility)throw std::runtime_error("incompatible-build");
   if(r.members.size()>=8)throw std::runtime_error("room-full");
   Member m;m.name=required(req,"name",16);m.token=random_text(43,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");m.uid=random_text(16,"0123456789abcdef");
   std::set<int> seats;for(const auto& [t,member]:r.members)seats.insert(member.seat);
   while(seats.count(m.seat))++m.seat;
   m.initials=optional(req,"initials",("P"+std::to_string(m.seat+1)).c_str());
   std::string token=m.token;r.members.emplace(token,m);tokens_[token]=r.code;
   for(auto& [t,member]:r.members)member.ready=false;
   ++r.revision;r.touched=std::chrono::steady_clock::now();
   return Json::dict({{"token",token},{"room",snapshot(r,token)}});
  }
  std::string token=required(req,"token",128);
  auto ti=tokens_.find(token);if(ti==tokens_.end())throw std::runtime_error("invalid-session");
  Room& r=rooms_.at(ti->second);state_for_log=r.state;r.touched=std::chrono::steady_clock::now();
  if(op=="state")return Json::dict({{"room",snapshot(r,token)}});
  if(op=="leave"){
   if(token==r.host){for(const auto& [t,m]:r.members)tokens_.erase(t);rooms_.erase(r.code);return Json::dict({{"closed",true}});}
   r.members.erase(token);tokens_.erase(token);++r.revision;return Json::dict({{"left",true}});
  }
  if(op=="start"){
   if(token!=r.host)throw std::runtime_error("host-only");
   if(r.flow!="garage" || r.state!="forming")throw std::runtime_error("event-started");
   r.state="mode";++r.revision;
   return Json::dict({{"room",snapshot(r,token)}});
  }
  if(op=="rules"){
   if(token!=r.host)throw std::runtime_error("host-only");
   if(r.flow!="garage" || r.state!="mode")throw std::runtime_error("event-started");
   if(!req.has("mode") || req.at("mode").kind!=Json::String || !req.has("rules") || req.at("rules").kind!=Json::Object)
    throw std::runtime_error("invalid-rules");
   const std::string mode=req.at("mode").str();
   if(std::set<std::string>{"grand-prix","balloon","shine","bob-omb"}.count(mode)==0)throw std::runtime_error("invalid-rules");
   r.mode=mode;r.rules=req.at("rules");r.state="characters";
   for(auto& [id,member]:r.members)member.ready=false;
   ++r.revision;
   return Json::dict({{"room",snapshot(r,token)}});
  }
  if(op!="preview" && (!req.has("revision") || req.at("revision").kind!=Json::Number || req.at("revision").number!=r.revision))
   throw std::runtime_error("stale-room");
  Member& member=r.members.at(token);
  if(op=="preview") {
   if(req.has("character") && (req.at("character").kind!=Json::Number || req.at("character").number<0 || req.at("character").number>20))throw std::runtime_error("invalid-character");
   return Json::dict({{"room",snapshot(r,token)}});
  }
  if(op=="ready"){
   if(!req.has("ready") || req.at("ready").kind!=Json::Boolean)throw std::runtime_error("invalid-ready");
   member.ready=req.at("ready").flag;
  }else if(op=="profile"){
   std::string initials=required(req,"initials",7);
   if(std::any_of(initials.begin(),initials.end(),[](unsigned char c){return !(c>='A'&&c<='Z') && !(c>='0'&&c<='9');}))throw std::runtime_error("invalid-initials");
   member.initials=initials;member.ready=false;
   if(r.flow=="garage")member.name=initials;
  }else if(op=="seat"){
   if(!req.has("seat") || req.at("seat").kind!=Json::Number || req.at("seat").number<0 || req.at("seat").number>=(r.layout=="mixed"?16:8))throw std::runtime_error("invalid-seat");
   int seat=(int)req.at("seat").number;
   for(const auto& [t,m]:r.members)if(t!=token && m.seat==seat)throw std::runtime_error("seat-occupied");
   if(r.layout=="mixed"){
    if(seat>=8){bool driver=false;for(const auto& [t,m]:r.members)if(t!=token && m.seat==seat-8)driver=true;if(!driver)throw std::runtime_error("driver-required");}
    if(member.seat<8 && seat!=member.seat)for(auto& [t,m]:r.members)if(t!=token && m.seat==member.seat+8)m.seat=member.seat;
   }
   member.seat=seat;
   for(auto& [t,m]:r.members)m.ready=false;
  }else if(op=="back"){
   if(token!=r.host)throw std::runtime_error("host-only");
   if(r.state=="course"){r.state="characters";for(auto& [id,m]:r.members)m.selected=false;}
   else if(r.state=="mode" || r.state=="characters"){
    r.state="forming";for(auto& [id,m]:r.members){m.ready=false;m.characters.clear();m.selected=false;m.kart_selected=false;}
   }else throw std::runtime_error("event-started");
  }else if(op=="character"){
   if(r.state!="characters" || !req.has("characters") || req.at("characters").kind!=Json::Array)throw std::runtime_error("invalid-character");
   const size_t needed=r.layout=="solo"?2:1;const auto& list=req.at("characters").list();if(list.size()>needed)throw std::runtime_error("invalid-character");
   std::vector<int> chosen;for(const auto& value:list){if(value.kind!=Json::Number||value.number<1||value.number>20)throw std::runtime_error("invalid-character");int id=(int)value.number;if(std::find(chosen.begin(),chosen.end(),id)!=chosen.end())throw std::runtime_error("invalid-character");chosen.push_back(id);}
   for(const auto& [id,other]:r.members)if(id!=token)for(int value:other.characters)if(std::find(chosen.begin(),chosen.end(),value)!=chosen.end())throw std::runtime_error("character-taken");
   member.characters=std::move(chosen);member.selected=false;
  }else if(op=="selection-kart"){
   if(r.state!="characters"||member.seat>=8||!req.has("kart")||req.at("kart").kind!=Json::Number||req.at("kart").number<0||req.at("kart").number>20)throw std::runtime_error("invalid-kart");
   member.kart=(int)req.at("kart").number;member.kart_selected=true;member.selected=false;
  }else if(op=="selection-edit"){
   if(r.state!="characters")throw std::runtime_error("event-started");member.selected=false;
  }else if(op=="selection-done"){
   if(r.state!="characters")throw std::runtime_error("event-started");const size_t needed=r.layout=="solo"?2:1;if(member.characters.size()!=needed)throw std::runtime_error("characters-incomplete");if(member.seat<8&&!member.kart_selected)throw std::runtime_error("kart-not-selected");member.selected=true;bool complete=r.members.size()>=2;for(const auto& [id,other]:r.members)if(!other.selected)complete=false;if(complete)r.state="course";
  }else if(op=="kart"){
   if(member.seat>=8||!req.has("choice")||req.at("choice").kind!=Json::Object)throw std::runtime_error("invalid-kart");const auto& choice=req.at("choice");for(const char* key:{"kart","driver","partner"})if(!choice.has(key)||choice.at(key).kind!=Json::Number)throw std::runtime_error("invalid-kart");int kart=(int)choice.at("kart").number,driver=(int)choice.at("driver").number,partner=(int)choice.at("partner").number;if(kart<0||kart>20||driver<1||driver>20||partner<1||partner>20||driver==partner)throw std::runtime_error("invalid-kart");member.kart=kart;member.driver=driver;member.partner=partner;member.kart_selected=true;member.ready=false;
  }else throw std::runtime_error("unsupported-operation");
  ++r.revision;
  return Json::dict({{"room",snapshot(r,token)}});

 }catch(const std::exception& e){
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_INFO,"TANDEM-EOS","native room op=%s state=%s error=%s",op_for_log.c_str(),state_for_log.c_str(),e.what());
#endif
  return Json::dict({{"error",e.what()}});
 }
}
}
