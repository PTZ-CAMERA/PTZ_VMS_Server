#include "onvif/CurlOnvifClient.h"
#include <boost/asio.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <curl/curl.h>
#include <tinyxml2.h>
extern "C" {
#include <libavutil/base64.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
#include <array>
#include <cmath>
#include <locale>
#include <ctime>
#include <iomanip>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
namespace vms {
namespace {
using tinyxml2::XMLElement;
const std::string DeviceNs = "http://www.onvif.org/ver10/device/wsdl";
const std::string PtzNs = "http://www.onvif.org/ver20/ptz/wsdl";
const std::string MediaNs = "http://www.onvif.org/ver10/media/wsdl";
std::string local(const char* name) {
    const std::string value = name ? name : "";
    const auto colon = value.find(':'); return value.substr(colon == std::string::npos ? 0 : colon + 1);
}
XMLElement* find(XMLElement* root, const std::string& name) {
    if (!root) return nullptr;
    if (local(root->Name()) == name) return root;
    for (auto* child = root->FirstChildElement(); child; child = child->NextSiblingElement())
        if (auto* found = find(child, name)) return found;
    return nullptr;
}
std::string value(XMLElement* root, const std::string& name) {
    auto* element = find(root, name); return element && element->GetText() ? element->GetText() : "";
}
void parse(tinyxml2::XMLDocument& doc, const std::string& text) {
    if (doc.Parse(text.data(), text.size()) != tinyxml2::XML_SUCCESS) throw std::runtime_error("Invalid ONVIF XML response");
}
std::string escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break; case '<': out += "&lt;"; break; case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break; case '\'': out += "&apos;"; break; default: out += c;
        }
    }
    return out;
}
struct UrlDeleter { void operator()(CURLU* p) const { curl_url_cleanup(p); } };
using Url = std::unique_ptr<CURLU, UrlDeleter>;
Url urlObject(const std::string& text) {
    Url result(curl_url());
    if (!result || curl_url_set(result.get(), CURLUPART_URL, text.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        throw std::runtime_error("Invalid camera URL");
    return result;
}
std::string part(CURLU* url, CURLUPart partName) {
    char* raw = nullptr;
    if (curl_url_get(url, partName, &raw, CURLU_URLDECODE) != CURLUE_OK) return {};
    std::unique_ptr<char, decltype(&curl_free)> owner(raw, &curl_free); return raw;
}
void httpUrl(const std::string& text) {
    if (text.size() > 2048 || text.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid ONVIF service URL");
    auto url = urlObject(text); const auto scheme = part(url.get(), CURLUPART_SCHEME);
    if ((scheme != "http" && scheme != "https") || part(url.get(), CURLUPART_HOST).empty()
        || !part(url.get(), CURLUPART_USER).empty() || !part(url.get(), CURLUPART_PASSWORD).empty())
        throw std::runtime_error("ONVIF service URL must be HTTP/HTTPS without embedded credentials");
}
std::string b64(const unsigned char* bytes, int size) {
    std::string out(AV_BASE64_SIZE(size), '\0'); av_base64_encode(out.data(), static_cast<int>(out.size()), bytes, size);
    out.resize(out.find('\0')); return out;
}
std::string security(const CameraInfo& camera) {
    if (camera.username.empty()) return {};
    std::array<unsigned char, 16> nonce{}; std::random_device random;
    for (auto& byte : nonce) byte = static_cast<unsigned char>(random());
    const auto now = std::time(nullptr); std::tm time{};
#ifdef _WIN32
    gmtime_s(&time, &now);
#else
    gmtime_r(&now, &time);
#endif
    std::ostringstream created; created << std::put_time(&time, "%Y-%m-%dT%H:%M:%SZ");
    const auto suffix = created.str() + camera.password;
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), &av_free);
    if (!sha || av_sha_init(sha.get(), 160) < 0) throw std::runtime_error("Cannot initialize ONVIF digest");
    av_sha_update(sha.get(), nonce.data(), nonce.size());
    av_sha_update(sha.get(), reinterpret_cast<const unsigned char*>(suffix.data()), suffix.size());
    std::array<unsigned char, 20> digest{}; av_sha_final(sha.get(), digest.data());
    return "<wsse:Security s:mustUnderstand=\"1\"><wsse:UsernameToken><wsse:Username>" + escape(camera.username)
        + "</wsse:Username><wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
        + b64(digest.data(), static_cast<int>(digest.size())) + "</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
        + b64(nonce.data(), static_cast<int>(nonce.size())) + "</wsse:Nonce><wsu:Created>" + created.str() + "</wsu:Created></wsse:UsernameToken></wsse:Security>";
}
struct CurlDeleter { void operator()(CURL* p) const { curl_easy_cleanup(p); } };
struct HeadersDeleter { void operator()(curl_slist* p) const { curl_slist_free_all(p); } };
std::size_t writeBody(char* data, std::size_t size, std::size_t count, void* target) noexcept {
    try {
        auto& body = *static_cast<std::string*>(target);
        if (size && count > (1024 * 1024) / size) return 0;
        const auto bytes = size * count; if (body.size() + bytes > 1024 * 1024) return 0;
        body.append(data, bytes); return bytes;
    } catch (...) { return 0; }
}
std::size_t headerBody(char* data, std::size_t size, std::size_t count, void* target) noexcept {
    const auto bytes = size * count;
    if (bytes >= 5 && std::string_view(data, 5) == "HTTP/") static_cast<std::string*>(target)->clear();
    return bytes;
}
int interrupt(void* target, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<const std::atomic<bool>*>(target)->load() ? 1 : 0;
}
struct SoapFault : std::runtime_error {
    bool streamSetup;
    explicit SoapFault(bool invalid) : std::runtime_error("ONVIF service returned a SOAP fault"), streamSetup(invalid) {}
};
}
CameraEndpoint parseCameraEndpoint(const std::string& text) {
    auto url = urlObject(text); return {part(url.get(), CURLUPART_HOST), part(url.get(), CURLUPART_USER), part(url.get(), CURLUPART_PASSWORD)};
}
std::string authenticatedRtspUrl(const std::string& text, const std::string& username, const std::string& password) {
    auto url = urlObject(text);
    if (part(url.get(), CURLUPART_SCHEME) != "rtsp" || part(url.get(), CURLUPART_HOST).empty()) throw std::runtime_error("ONVIF did not return a valid RTSP URI");
    if (!username.empty()) {
        if (curl_url_set(url.get(), CURLUPART_USER, username.c_str(), CURLU_URLENCODE) != CURLUE_OK
            || curl_url_set(url.get(), CURLUPART_PASSWORD, password.c_str(), CURLU_URLENCODE) != CURLUE_OK)
            throw std::runtime_error("Invalid RTSP credentials");
    }
    char* raw = nullptr;
    if (curl_url_get(url.get(), CURLUPART_URL, &raw, 0) != CURLUE_OK) throw std::runtime_error("Cannot construct RTSP input URI");
    std::unique_ptr<char, decltype(&curl_free)> owner(raw, &curl_free); return raw;
}
CurlOnvifClient::CurlOnvifClient(const std::atomic<bool>& cancelled) : cancelled_(cancelled) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Cannot initialize ONVIF HTTP runtime");
}
CurlOnvifClient::~CurlOnvifClient() { curl_global_cleanup(); }
std::string CurlOnvifClient::soap(const CameraInfo& camera, const std::string& url, const std::string& ns, const std::string& operation, const std::string& body, const std::string& addressing, long timeoutMs) {
    httpUrl(url);
    if (parseCameraEndpoint(url).host != parseCameraEndpoint(camera.onvifUrl).host) throw std::runtime_error("ONVIF service endpoint changed camera host");
    const auto payload = "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:p=\"" + ns
        + "\" xmlns:tt=\"http://www.onvif.org/ver10/schema\" xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><s:Header>"
        + addressing + security(camera) + "</s:Header><s:Body><p:" + operation + '>' + body + "</p:" + operation + "></s:Body></s:Envelope>";
    std::unique_ptr<CURL, CurlDeleter> handle(curl_easy_init()); if (!handle) throw std::bad_alloc();
    const auto contentType = "Content-Type: application/soap+xml; charset=utf-8; action=\"" + ns + '/' + operation + "\"";
    std::unique_ptr<curl_slist, HeadersDeleter> headers(curl_slist_append(nullptr, contentType.c_str())); if (!headers) throw std::bad_alloc();
    std::string response;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str()); curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    if (!camera.username.empty()) {
        curl_easy_setopt(handle.get(), CURLOPT_USERNAME, camera.username.c_str()); curl_easy_setopt(handle.get(), CURLOPT_PASSWORD, camera.password.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_DIGEST));
    }
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload.c_str()); curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, 2000L); curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https"); curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, &writeBody); curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, &headerBody); curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, &interrupt);
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &cancelled_);
    const auto result = curl_easy_perform(handle.get());
    if (result != CURLE_OK) throw std::runtime_error("ONVIF HTTP request failed or timed out");
    long status = 0; curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status == 401 || status == 403) throw std::runtime_error("ONVIF authentication failed");
    tinyxml2::XMLDocument doc; parse(doc, response);
    if (auto* fault = find(doc.RootElement(), "Fault")) {
        const auto code = value(find(fault, "Subcode"), "Value");
        throw SoapFault(code.find("InvalidArgVal") != std::string::npos);
    }
    if (status != 200 || !find(doc.RootElement(), operation + "Response")) throw std::runtime_error("Unexpected ONVIF service response");
    return response;
}
std::vector<CameraInfo> CurlOnvifClient::discoverDevices(std::chrono::milliseconds timeout) {
    namespace net = boost::asio; using udp = net::ip::udp;
    net::io_context io; udp::socket socket(io); socket.open(udp::v4()); socket.bind(udp::endpoint(udp::v4(), 0)); socket.non_blocking(true);
    const auto id = "urn:uuid:" + boost::uuids::to_string(boost::uuids::random_generator()());
    const auto probe = "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><s:Header><a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action><a:MessageID>" + id
        + "</a:MessageID><a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To><a:ReplyTo><a:Address>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:Address></a:ReplyTo></s:Header><s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></s:Body></s:Envelope>";
    socket.send_to(net::buffer(probe), udp::endpoint(net::ip::make_address("239.255.255.250"), 3702));
    std::vector<CameraInfo> cameras; std::set<std::string> seen;
    const auto end = std::chrono::steady_clock::now() + timeout;
    std::array<char, 65536> buffer{};
    while (!cancelled_.load() && std::chrono::steady_clock::now() < end && cameras.size() < 32) {
        udp::endpoint sender; boost::system::error_code error;
        const auto size = socket.receive_from(net::buffer(buffer), sender, 0, error);
        if (error == net::error::would_block || error == net::error::try_again) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        if (error) break;
        tinyxml2::XMLDocument doc;
        if (doc.Parse(buffer.data(), size) != tinyxml2::XML_SUCCESS || value(doc.RootElement(), "RelatesTo") != id) continue;
        auto* matches = find(doc.RootElement(), "ProbeMatches"); if (!matches) continue;
        for (auto* match = matches->FirstChildElement(); match && cameras.size() < 32; match = match->NextSiblingElement()) {
            if (local(match->Name()) != "ProbeMatch") continue;
            std::istringstream addresses(value(match, "XAddrs")); std::string url;
            while (addresses >> url) {
                try {
                    httpUrl(url);
                    if (seen.insert(url).second) { CameraInfo camera; camera.onvifUrl = url; camera.ipAddress = parseCameraEndpoint(url).host; camera.name = "ONVIF camera"; cameras.push_back(std::move(camera)); }
                } catch (const std::exception&) {}
                if (cameras.size() >= 32) break;
            }
        }
    }
    return cameras;
}
DeviceInformation CurlOnvifClient::getDeviceInformation(const CameraInfo& camera) {
    tinyxml2::XMLDocument doc; parse(doc, soap(camera, camera.onvifUrl, DeviceNs, "GetDeviceInformation", {}));
    auto* root = doc.RootElement(); return {value(root, "Manufacturer"), value(root, "Model"), value(root, "FirmwareVersion"), value(root, "SerialNumber"), value(root, "HardwareId")};
}
OnvifCapabilities CurlOnvifClient::getCapabilities(const CameraInfo& camera) {
    tinyxml2::XMLDocument doc; parse(doc, soap(camera, camera.onvifUrl, DeviceNs, "GetCapabilities", "<p:Category>All</p:Category>"));
    OnvifCapabilities caps{camera.onvifUrl, value(find(doc.RootElement(), "Media"), "XAddr"), value(find(doc.RootElement(), "PTZ"), "XAddr"), value(find(doc.RootElement(), "Events"), "XAddr")};
    capabilities_[camera.onvifUrl] = caps; return caps;
}
std::vector<MediaProfile> CurlOnvifClient::getProfiles(const CameraInfo& camera) {
    auto copy = camera; const auto caps = capabilities_.count(camera.onvifUrl) ? capabilities_.at(camera.onvifUrl) : getCapabilities(camera); if (caps.mediaUrl.empty()) throw std::runtime_error("Camera has no ONVIF Media1 service");
    tinyxml2::XMLDocument doc; parse(doc, soap(copy, caps.mediaUrl, MediaNs, "GetProfiles", {}));
    std::vector<MediaProfile> profiles;
    auto* response = find(doc.RootElement(), "GetProfilesResponse");
    for (auto* profile = response ? response->FirstChildElement() : nullptr; profile; profile = profile->NextSiblingElement()) {
        if (local(profile->Name()) != "Profiles" || !profile->Attribute("token")) continue;
        auto* ptz = find(profile, "PTZConfiguration");
        profiles.push_back({profile->Attribute("token"), value(profile, "Name"), ptz != nullptr,
            ptz && ptz->Attribute("token") ? ptz->Attribute("token") : ""});
    }
    return profiles;
}
std::string CurlOnvifClient::getStreamUri(const CameraInfo& camera, const std::string& token) {
    const auto caps = capabilities_.count(camera.onvifUrl) ? capabilities_.at(camera.onvifUrl) : getCapabilities(camera); if (caps.mediaUrl.empty()) throw std::runtime_error("Camera has no ONVIF Media1 service");
    auto fetch = [&](const char* streamType) {
        const auto body = std::string("<p:StreamSetup><tt:Stream>") + streamType + "</tt:Stream><tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></p:StreamSetup><p:ProfileToken>" + escape(token) + "</p:ProfileToken>";
        tinyxml2::XMLDocument doc; parse(doc, soap(camera, caps.mediaUrl, MediaNs, "GetStreamUri", body));
        const auto uri = value(doc.RootElement(), "Uri");
        if (parseCameraEndpoint(uri).host != parseCameraEndpoint(camera.onvifUrl).host) throw std::runtime_error("ONVIF stream URI changed camera host");
        return authenticatedRtspUrl(uri, camera.username, camera.password);
    };
    try { return fetch("RTP-Unicast"); }
    catch (const SoapFault& fault) { if (!fault.streamSetup) throw; return fetch("RTP_unicast"); }
}
OnvifCapabilities CurlOnvifClient::getServices(const CameraInfo& camera) {
    tinyxml2::XMLDocument doc;
    parse(doc, soap(camera, camera.onvifUrl, DeviceNs, "GetServices", "<p:IncludeCapability>false</p:IncludeCapability>"));
    OnvifCapabilities caps; caps.deviceUrl = camera.onvifUrl;
    auto* response = find(doc.RootElement(), "GetServicesResponse");
    for (auto* service = response ? response->FirstChildElement() : nullptr; service; service = service->NextSiblingElement()) {
        const auto ns = value(service, "Namespace"), url = value(service, "XAddr");
        if (ns == MediaNs) caps.mediaUrl = url;
        if (ns == PtzNs) caps.ptzUrl = url;
        if (ns == "http://www.onvif.org/ver10/events/wsdl") caps.eventsUrl = url;
    }
    capabilities_[camera.onvifUrl] = caps; return caps;
}
PtzConfiguration CurlOnvifClient::getConfigurationOptions(const CameraInfo& camera, const std::string& token) {
    const auto services = getServices(camera);
    if (services.ptzUrl.empty()) throw std::runtime_error("No ONVIF PTZ service");
    const auto profiles = getProfiles(camera);
    std::string config;
    for (const auto& profile : profiles) if (profile.token == token) config = profile.ptzConfigurationToken;
    if (config.empty()) throw std::runtime_error("Profile has no PTZ configuration");
    tinyxml2::XMLDocument doc;
    parse(doc, soap(camera, services.ptzUrl, PtzNs, "GetConfigurationOptions", "<p:ConfigurationToken>" + escape(config) + "</p:ConfigurationToken>"));
    auto space = [&](const char* name, const std::string& wanted) {
        PtzSpace out;
        auto* spaces = find(doc.RootElement(), "Spaces");
        for (auto* node = spaces ? spaces->FirstChildElement() : nullptr; node; node = node->NextSiblingElement()) {
            if (local(node->Name()) != name || value(node, "URI") != wanted) continue;
            out.uri = wanted;
            out.minX = std::stof(value(find(node, "XRange"), "Min")); out.maxX = std::stof(value(find(node, "XRange"), "Max"));
            out.minY = std::stof(value(find(node, "YRange"), "Min")); out.maxY = std::stof(value(find(node, "YRange"), "Max"));
            if (!std::isfinite(out.minX) || !std::isfinite(out.maxX) || !std::isfinite(out.minY) || !std::isfinite(out.maxY)
                || out.minX > 0 || out.maxX < 0 || out.minY > 0 || out.maxY < 0) throw std::runtime_error("Invalid PTZ coordinate range");
            break;
        }
        return out;
    };
    const std::string prefix = "http://www.onvif.org/ver10/tptz/PanTiltSpaces/";
    PtzConfiguration result{services.ptzUrl, space("ContinuousPanTiltVelocitySpace", prefix + "VelocityGenericSpace"),
        space("AbsolutePanTiltPositionSpace", prefix + "PositionGenericSpace")};
    if (result.velocity.uri.empty()) throw std::runtime_error("Generic continuous PTZ space unavailable");
    // PT1S is required by our hold-to-move protocol. Reject cameras that cannot honor it.
    auto duration = [](const std::string& text) {
        if (text.size() < 4 || text.substr(0, 2) != "PT" || text.back() != 'S') throw std::runtime_error("Unsupported PTZ timeout range");
        std::size_t used = 0; const auto number = text.substr(2, text.size() - 3); const auto result = std::stod(number, &used);
        if (used != number.size() || !std::isfinite(result)) throw std::runtime_error("Invalid PTZ timeout range");
        return result;
    };
    auto* timeout = find(doc.RootElement(), "PTZTimeout");
    if (!timeout || duration(value(timeout, "Min")) > 1 || duration(value(timeout, "Max")) < 1)
        throw std::runtime_error("PT1S movement timeout unsupported");
    // 카메라가 실제 광고한 MoveAndTrack 유형만 확인한다. 미지원 조회가 영상/PTZ 등록을 막지는 않는다.
    try {
        tinyxml2::XMLDocument caps;
        parse(caps, soap(camera, services.ptzUrl, PtzNs, "GetServiceCapabilities", ""));
        const auto* node = find(caps.RootElement(), "Capabilities");
        const char* declared = node ? node->Attribute("MoveAndTrack") : nullptr;
        std::istringstream types(declared ? declared : ""); std::string type;
        while (types >> type) if (type == "PTZVector") result.supportsTracking = true;
    } catch (const std::exception&) { result.supportsTracking = false; }
    setPtzConfiguration(camera, token, result); return result;
}
void CurlOnvifClient::setPtzConfiguration(const CameraInfo& camera, const std::string& token, const PtzConfiguration& options) {
    ptz_[{camera.onvifUrl, token}] = options;
}
Result CurlOnvifClient::ptzCommand(const CameraInfo& camera, const std::string& token, const std::string& operation, float x, float y) {
    try {
        const auto& options = ptz_.at({camera.onvifUrl, token});
        std::string body = "<p:ProfileToken>" + escape(token) + "</p:ProfileToken>";
        if (operation == "Stop") body += "<p:PanTilt>true</p:PanTilt><p:Zoom>false</p:Zoom>";
        else if (operation != "MoveAndStartTracking") {
            const bool moving = operation == "ContinuousMove";
            const auto& space = moving ? options.velocity : options.position;
            if (space.uri.empty() || !std::isfinite(x) || !std::isfinite(y) || x < space.minX || x > space.maxX || y < space.minY || y > space.maxY)
                return {false, "PTZ_OUT_OF_RANGE"};
            std::ostringstream xy; xy.imbue(std::locale::classic()); xy << " x=\"" << x << "\" y=\"" << y << "\" space=\"" << escape(space.uri) << "\"/>";
            const std::string tag = moving ? "Velocity" : "Position";
            body += "<p:" + tag + "><tt:PanTilt" + xy.str() + "</p:" + tag + ">";
            if (moving) body += "<p:Timeout>PT1S</p:Timeout>";
        }
        soap(camera, options.endpoint, PtzNs, operation, body);
        return {true, {}};
    } catch (const SoapFault&) { return {false, "ONVIF_FAULT"}; }
    catch (const std::exception&) { return {false, "ONVIF_COMMUNICATION_ERROR"}; }
}
Result CurlOnvifClient::continuousMove(const CameraInfo& c, const std::string& t, float x, float y) { return ptzCommand(c, t, "ContinuousMove", x, y); }
Result CurlOnvifClient::relativeMove(const CameraInfo&, const std::string&, float, float) { return {false, "NOT_SUPPORTED"}; }
Result CurlOnvifClient::absoluteMove(const CameraInfo& c, const std::string& t, float x, float y) { return ptzCommand(c, t, "AbsoluteMove", x, y); }
Result CurlOnvifClient::stop(const CameraInfo& c, const std::string& t) { return ptzCommand(c, t, "Stop"); }
Result CurlOnvifClient::startTracking(const CameraInfo& c, const std::string& t) {
    // TargetPosition 생략은 현재 위치에서 시작한다. 중앙 이동이나 GPIO 제어를 VMS에서 하지 않는다.
    const auto found = ptz_.find({c.onvifUrl, t});
    if (found == ptz_.end() || !found->second.supportsTracking) return {false, "TRACKING_NOT_SUPPORTED"};
    return ptzCommand(c, t, "MoveAndStartTracking");
}
PtzStatus CurlOnvifClient::getStatus(const CameraInfo&, const std::string&) { throw std::runtime_error("PTZ status adapter is not implemented"); }
}
