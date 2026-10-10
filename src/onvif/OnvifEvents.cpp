#include "onvif/CurlOnvifClient.h"
#include "core/UtcTimestamp.h"
#include <tinyxml2.h>
#include <atomic>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
namespace vms {
namespace {
using tinyxml2::XMLElement;
const std::string Events = "http://www.onvif.org/ver10/events/wsdl";
const std::string Wsn = "http://docs.oasis-open.org/wsn/b-2";
const std::string Address = "http://www.w3.org/2005/08/addressing";
const std::string Schema = "http://www.onvif.org/ver10/schema";
std::string local(const char* name) {
    std::string text = name ? name : ""; const auto colon = text.find(':');
    return text.substr(colon == std::string::npos ? 0 : colon + 1);
}
std::string ns(const XMLElement* element) {
    const std::string name = element->Name(); const auto colon = name.find(':');
    const auto attribute = colon == std::string::npos ? "xmlns" : "xmlns:" + name.substr(0, colon);
    for (auto* node = element; node; node = node->Parent() ? node->Parent()->ToElement() : nullptr)
        if (const char* uri = node->Attribute(attribute.c_str())) return uri;
    return {};
}
XMLElement* find(XMLElement* node, const std::string& name, const std::string& uri = {}) {
    if (!node) return nullptr;
    if (local(node->Name()) == name && (uri.empty() || ns(node) == uri)) return node;
    for (auto* child = node->FirstChildElement(); child; child = child->NextSiblingElement())
        if (auto* result = find(child, name, uri)) return result;
    return nullptr;
}
std::string text(XMLElement* node) { return node && node->GetText() ? node->GetText() : ""; }
std::string attr(XMLElement* node, const char* name) { return node && node->Attribute(name) ? node->Attribute(name) : ""; }
void parse(tinyxml2::XMLDocument& doc, const std::string& xml) {
    if (doc.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS) throw std::runtime_error("Invalid Events XML");
}
std::string escape(const std::string& value) {
    std::string result; for (char c : value) {
        if (c == '&') result += "&amp;"; else if (c == '<') result += "&lt;";
        else if (c == '>') result += "&gt;"; else result += c;
    } return result;
}
std::string headers(const std::string& endpoint, const std::string& action, const std::string& reference = {}) {
    static std::atomic<unsigned long long> sequence{0};
    return "<wsa:Action>" + action + "</wsa:Action><wsa:MessageID>urn:vms:" + std::to_string(utcNowMs()) + ":" + std::to_string(++sequence)
        + "</wsa:MessageID><wsa:To>" + escape(endpoint) + "</wsa:To><wsa:ReplyTo><wsa:Address>"
        + Address + "/anonymous</wsa:Address></wsa:ReplyTo>" + reference;
}
void times(XMLElement* root, EventSubscription& sub) {
    const auto current = parseUtcMs(text(find(root, "CurrentTime")));
    const auto expiry = parseUtcMs(text(find(root, "TerminationTime")));
    if (!current || !expiry || *expiry <= *current) throw std::runtime_error("Invalid subscription lifetime");
    sub.currentMs = *current; sub.terminationMs = *expiry;
}
// EPR 참조 파라미터가 상위 Envelope의 namespace를 사용해도 다음 SOAP에서 유효하도록 보존한다.
std::string referenceXml(XMLElement* root) {
    std::string result;
    for (auto* child = root ? root->FirstChildElement() : nullptr; child; child = child->NextSiblingElement()) {
        tinyxml2::XMLDocument copy; auto* element = child->DeepClone(&copy)->ToElement(); copy.InsertEndChild(element);
        std::map<std::string,std::string> namespaces;
        for (auto* node = child; node; node = node->Parent() ? node->Parent()->ToElement() : nullptr)
            for (auto* a = node->FirstAttribute(); a; a = a->Next())
                if (std::string(a->Name()).rfind("xmlns",0) == 0) namespaces.emplace(a->Name(),a->Value());
        for (const auto& item : namespaces) element->SetAttribute(item.first.c_str(),item.second.c_str());
        element->SetAttribute("xmlns:wsa",Address.c_str()); element->SetAttribute("wsa:IsReferenceParameter","true");
        tinyxml2::XMLPrinter printer; element->Accept(&printer); result += printer.CStr();
    } return result;
}
}
std::vector<OnvifEventMessage> parseEventMessages(const std::string& xml) {
    tinyxml2::XMLDocument doc; parse(doc,xml); std::vector<OnvifEventMessage> messages;
    auto* response = find(doc.RootElement(),"PullMessagesResponse",Events);
    if (!response) throw std::runtime_error("Missing PullMessagesResponse");
    for (auto* node = response->FirstChildElement(); node; node = node->NextSiblingElement()) {
        if (local(node->Name()) != "NotificationMessage" || ns(node) != Wsn) continue;
        if (messages.size() >= 128) throw std::runtime_error("Event message limit exceeded");
        OnvifEventMessage out;
        out.topic = text(find(node,"Topic",Wsn));
        const auto first=out.topic.find_first_not_of(" \t\r\n");
        if (first==std::string::npos) continue;
        out.topic=out.topic.substr(first,out.topic.find_last_not_of(" \t\r\n")-first+1);
        auto* message = find(node,"Message",Schema); if (!message || out.topic.size() > 512) continue;
        out.sourceTimestamp = attr(message,"UtcTime"); out.sourceMs = parseUtcMs(out.sourceTimestamp);
        out.operation = attr(message,"PropertyOperation");
        auto items = [](XMLElement* group) {
            auto result = nlohmann::json::object();
            for (auto* item = group ? group->FirstChildElement() : nullptr; item; item = item->NextSiblingElement()) {
                if (local(item->Name()) != "SimpleItem" || ns(item) != Schema) continue;
                const auto name = attr(item,"Name"), value = attr(item,"Value");
                if (name.empty() || name.size() > 128 || value.size() > 2048 || result.size() >= 64 || result.contains(name))
                    throw std::runtime_error("Invalid event SimpleItem");
                result[name] = value;
            } return result;
        };
        out.source = items(find(message,"Source",Schema)); out.data = items(find(message,"Data",Schema));
        messages.push_back(std::move(out));
    } return messages;
}
nlohmann::json CurlOnvifClient::getEventProperties(const CameraInfo& camera, const std::string& endpoint) {
    tinyxml2::XMLDocument doc;
    parse(doc,soap(camera,endpoint,Events,"GetEventProperties",{},headers(endpoint,Events + "/EventPortType/GetEventPropertiesRequest")));
    auto result = nlohmann::json{{"topics",nlohmann::json::array()},{"fields",nlohmann::json::array()}};
    auto walk = [&](auto&& self,XMLElement* node,std::string path) -> void {
        if (!node) return;
        bool topic = false;
        for (auto* a=node->FirstAttribute();a;a=a->Next())
            if (local(a->Name())=="topic" && std::string(a->Value())=="true") topic=true;
        // Pi는 Analytics 부모에 topic=true를 붙이지 않는다. 부모 QName까지 경로에 포함한다.
        if (local(node->Name())!="TopicSet" && local(node->Name())!="MessageDescription" && ns(node)!="http://www.onvif.org/ver10/schema")
            path += (path.empty() ? "" : "/") + local(node->Name());
        if (topic) result["topics"].push_back(path);
        if (local(node->Name()) == "SimpleItemDescription" && result["fields"].size()<256)
            result["fields"].push_back({{"topic",path},{"name",attr(node,"Name")},{"type",attr(node,"Type")}});
        for (auto* child = node->FirstChildElement(); child; child = child->NextSiblingElement()) self(self,child,path);
    };
    walk(walk,find(doc.RootElement(),"TopicSet"),{}); return result;
}
EventSubscription CurlOnvifClient::createPullPoint(const CameraInfo& camera, const std::string& endpoint) {
    tinyxml2::XMLDocument doc;
    parse(doc,soap(camera,endpoint,Events,"CreatePullPointSubscription","<p:InitialTerminationTime>PT60S</p:InitialTerminationTime>",
        headers(endpoint,Events + "/EventPortType/CreatePullPointSubscriptionRequest")));
    EventSubscription result; auto* ref = find(doc.RootElement(),"SubscriptionReference");
    result.endpoint = text(find(ref,"Address",Address));
    if (result.endpoint.empty()) throw std::runtime_error("Missing subscription endpoint");
    result.referenceParameters = referenceXml(find(ref,"ReferenceParameters",Address));
    times(doc.RootElement(),result); return result;
}
std::vector<OnvifEventMessage> CurlOnvifClient::pullMessages(const CameraInfo& camera,const EventSubscription& sub,unsigned seconds,unsigned limit) {
    return parseEventMessages(soap(camera,sub.endpoint,Events,"PullMessages","<p:Timeout>PT" + std::to_string(seconds)
        + "S</p:Timeout><p:MessageLimit>" + std::to_string(limit) + "</p:MessageLimit>",
        headers(sub.endpoint,Events + "/PullPointSubscription/PullMessagesRequest",sub.referenceParameters),static_cast<long>(seconds)*1000 + 3000));
}
void CurlOnvifClient::renew(const CameraInfo& camera,EventSubscription& sub) {
    tinyxml2::XMLDocument doc;
    parse(doc,soap(camera,sub.endpoint,Wsn,"Renew","<p:TerminationTime>PT60S</p:TerminationTime>",
        headers(sub.endpoint,"http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest",sub.referenceParameters)));
    times(doc.RootElement(),sub);
}
void CurlOnvifClient::unsubscribe(const CameraInfo& camera,const EventSubscription& sub) {
    soap(camera,sub.endpoint,Wsn,"Unsubscribe",{},headers(sub.endpoint,"http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest",sub.referenceParameters),1000);
}
std::optional<std::int64_t> CurlOnvifClient::getDeviceUtcMs(const CameraInfo& camera) {
    tinyxml2::XMLDocument doc;
    parse(doc,soap(camera,camera.onvifUrl,"http://www.onvif.org/ver10/device/wsdl","GetSystemDateAndTime",{}));
    auto* utc = find(doc.RootElement(),"UTCDateTime",Schema); if (!utc) return {};
    auto field = [&](const char* name) { return text(find(utc,name,Schema)); };
    std::ostringstream iso;
    try {
        iso << std::setw(4) << std::setfill('0') << std::stoi(field("Year")) << '-'
            << std::setw(2) << std::stoi(field("Month")) << '-' << std::setw(2) << std::stoi(field("Day")) << 'T'
            << std::setw(2) << std::stoi(field("Hour")) << ':' << std::setw(2) << std::stoi(field("Minute")) << ':' << std::setw(2) << std::stoi(field("Second")) << 'Z';
    } catch (...) { return {}; }
    return parseUtcMs(iso.str());
}
}
