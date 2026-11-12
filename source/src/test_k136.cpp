#include "rooms_native.h"
#include <iostream>
#include <stdexcept>
using mkdd::online::Json;
using mkdd::native_rooms::Rooms;
static void check(bool v,const char* msg){if(!v)throw std::runtime_error(msg);}
int main(){
 Rooms r;
 auto host=r.request(Json::dict({{"op","create"},{"name","Host"},{"compatibility","build-1"}}));
 auto token=host.at("token").str();
 auto code=host.at("room").at("code").str();
 auto friend_=r.request(Json::dict({{"op","join"},{"name","Friend"},{"compatibility","build-1"},{"code",code}}));
 auto ft=friend_.at("token").str();
 auto rev=friend_.at("room").at("revision").number;
 auto ready=r.request(Json::dict({{"op","ready"},{"token",ft},{"revision",rev},{"ready",true}}));
 check(ready.at("room").at("revision").number==rev+1,"ready revision");
 auto stale=r.request(Json::dict({{"op","ready"},{"token",ft},{"revision",rev},{"ready",false}}));
 check(stale.at("error").str()=="stale-room","stale revision");
 rev=ready.at("room").at("revision").number;
 auto profile=r.request(Json::dict({{"op","profile"},{"token",ft},{"revision",rev},{"initials","ABC"}}));
 check(profile.at("room").at("revision").number==rev+1,"profile revision");
 rev=profile.at("room").at("revision").number;
 auto seat=r.request(Json::dict({{"op","seat"},{"token",ft},{"revision",rev},{"seat",7}}));
 check(seat.at("room").at("revision").number==rev+1,"seat revision");
 auto occupied=r.request(Json::dict({{"op","seat"},{"token",token},{"revision",rev+1},{"seat",7}}));
 check(occupied.at("error").str()=="seat-occupied","seat conflict");
 std::cout<<"K136: READY/PROFILE/SEAT/REVISION OK (TESTE LOCAL)\\n";
}
