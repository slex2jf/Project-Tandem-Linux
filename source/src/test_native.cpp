#include "rooms_native.h"
#include <iostream>
#include <stdexcept>
using namespace mkdd::native_rooms;
int main(){
 Rooms rooms;
 auto a=rooms.request(Json::dict({{"op","create"},{"name","Linux"},{"compatibility","GM4E01"}}));
 if(a.has("error"))throw std::runtime_error(a.at("error").str());
 auto b=rooms.request(Json::dict({{"op","join"},{"name","Android"},{"compatibility","GM4E01"},{"code",a.at("room").at("code")}}));
 if(b.has("error"))throw std::runtime_error(b.at("error").str());
 auto state=rooms.request(Json::dict({{"op","state"},{"token",a.at("token")}}));
 if(state.at("room").at("members").list().size()!=2)throw std::runtime_error("members mismatch");
 auto leave=rooms.request(Json::dict({{"op","leave"},{"token",b.at("token")}}));
 if(!leave.at("left").boolean())throw std::runtime_error("leave failed");
 std::cout<<"K135: CREATE/JOIN/STATE/LEAVE OK (TESTE LOCAL)\n";
}
