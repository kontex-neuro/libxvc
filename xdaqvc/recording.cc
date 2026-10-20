#include "recording.h"
#include <cpr/cpr.h>
#include <cstdlib>
#include <format>
#include <stdexcept>

namespace xvc {
std::string device_host() {
    auto value=std::getenv("THORVISION_DEVICE_HOST");
    return value && *value ? value : "192.168.177.100";
}
int device_port() {
    auto value=std::getenv("THORVISION_DEVICE_PORT");
    if(value) {try {auto p=std::stoi(value);if(p>0 && p<=65535)return p;}catch(...) {}}
    return 8000;
}
std::string device_url() {return std::format("http://{}:{}",device_host(),device_port());}
nlohmann::json RecordingProfile::json() const {
    return {{"codec",codec},{"width",width},{"height",height},{"fps_n",fps_n},{"fps_d",fps_d}};
}
RecordingProfile RecordingProfile::parse(const nlohmann::json& j) {
    RecordingProfile p{j.at("codec"),j.at("width"),j.at("height"),j.at("fps_n"),j.at("fps_d")};
    if((p.codec!="h265" && p.codec!="jpeg") || p.width<=0 || p.height<=0 || p.fps_n<=0 || p.fps_d<=0)
        throw std::invalid_argument("Invalid recording profile");
    return p;
}
RecordingReply RecordingClient::request(std::string_view method,std::string_view path,const nlohmann::json& body,int timeout_ms) const {
    try {
        cpr::Session session;
        session.SetUrl(cpr::Url{base_+std::string(path)});
        session.SetTimeout(cpr::Timeout{timeout_ms});
        session.SetConnectTimeout(cpr::ConnectTimeout{2000});
        session.SetHeader(cpr::Header{{"Content-Type","application/json"}});
        if(method!="GET")session.SetBody(cpr::Body{body.dump()});
        auto response=method=="GET"?session.Get():method=="PUT"?session.Put():session.Post();
        RecordingReply result;result.status=static_cast<int>(response.status_code);
        if(!response.text.empty())result.state=nlohmann::json::parse(response.text);
        if(result.status!=200) {
            auto detail=result.state.value("detail",nlohmann::json{});
            result.error=detail.is_string()?detail.get<std::string>():!detail.is_null()?detail.dump():
                response.error.message.empty()?"Device recording request failed":response.error.message;
        }
        else if(!result.state.is_object() || result.state.value("version",0)!=1)
            result.error="Unsupported device recording protocol";
        return result;
    }catch(const std::exception& error){return {{},error.what(),0};}
}
RecordingReply RecordingClient::state() const {return request("GET","/recording");}
RecordingReply RecordingClient::apply(const nlohmann::json& value) const {return request("PUT","/recording/config",value,15000);}
RecordingReply RecordingClient::storage(std::string_view operation,const nlohmann::json& token) const {
    if(operation!="eject" && operation!="resume" && operation!="check" && operation!="repair")
        return {{},"Unknown storage operation",400};
    return request("POST",std::string("/recording/storage/")+std::string(operation),token,10000);
}
}
