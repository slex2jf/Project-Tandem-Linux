#pragma once
#include "online_json.h"
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
namespace mkdd::native_rooms {
using mkdd::online::Json;
struct Member {std::string token,name,initials,uid;int seat=0;bool ready=false;std::vector<int> characters;bool selected=false;bool kart_selected=false;int kart=0,driver=7,partner=8;};
struct Room {std::string code,compatibility,host,mode,layout,flow,state="forming";Json rules;std::map<std::string,Member> members;int revision=1;std::chrono::steady_clock::time_point touched=std::chrono::steady_clock::now();};
class Rooms {
 std::mutex mutex_;
 std::map<std::string,Room> rooms_;
 std::map<std::string,std::string> tokens_;
 static std::string random_text(size_t length,const std::string& alphabet);
 static std::string required(const Json& value,const char* key,size_t limit);
 static std::string optional(const Json& value,const char* key,const char* fallback);
 static Json snapshot(const Room& room,const std::string& token);
 public:
 Json request(const Json& request);
};
}
