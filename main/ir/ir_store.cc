// 红外码库持久化实现（NVS 单 blob + cJSON）
#include "ir_store.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace stackchan_ir {

namespace {
const char* kTag = "IR.Store";
const char* kNvsNamespace = "stackchan_ir";
const char* kNvsKey = "irlib_v1";
}  // namespace

bool IrStore::Init() {
    // NVS 由应用层初始化（settings.cc 已做 nvs_flash_init），此处仅打开命名空间
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs_handle_);
    if (err == ESP_OK) {
        nvs_ok_ = true;
    } else {
        ESP_LOGW(kTag, "nvs_open failed: %d (store 不可用，码库仅内存)", err);
    }
    Load();
    loaded_ = true;
    ESP_LOGI(kTag, "Init: %d devices loaded", (int)devices_.size());
    return nvs_ok_;
}

bool IrStore::Load() {
    devices_.clear();
    if (!nvs_ok_) return false;
    // 先查长度
    size_t len = 0;
    esp_err_t err = nvs_get_blob(nvs_handle_, kNvsKey, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;  // 首次运行
    }
    if (err != ESP_OK || len == 0) {
        ESP_LOGW(kTag, "nvs_get_blob failed: %d", err);
        return false;
    }
    std::string json(len, '\0');
    err = nvs_get_blob(nvs_handle_, kNvsKey, json.data(), &len);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_blob (data) failed: %d", err);
        return false;
    }
    json.resize(len);
    return ParseJson(json);
}

bool IrStore::Save() const {
    if (!nvs_ok_) return false;
    std::string json = ExportToJson();
    esp_err_t err = nvs_set_blob(nvs_handle_, kNvsKey, json.data(), json.size());
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_set_blob failed: %d", err);
        return false;
    }
    err = nvs_commit(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_commit failed: %d", err);
        return false;
    }
    return true;
}

bool IrStore::AddDevice(const DeviceInfo& dev) {
    if (!dev.Valid()) return false;
    if (devices_.count(dev.id) != 0) {
        devices_[dev.id].type = dev.type;
        devices_[dev.id].name = dev.name;
    } else {
        devices_[dev.id] = dev;
    }
    return Save();
}

bool IrStore::RemoveDevice(const std::string& id) {
    if (devices_.erase(id) == 0) return false;
    return Save();
}

bool IrStore::SetKey(const std::string& device_id, const std::string& key, const IrCode& code) {
    auto it = devices_.find(device_id);
    if (it == devices_.end() || key.empty() || !code.Valid()) return false;
    it->second.keys[key] = code;
    return Save();
}

bool IrStore::RemoveKey(const std::string& device_id, const std::string& key) {
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return false;
    if (it->second.keys.erase(key) == 0) return false;
    return Save();
}

bool IrStore::SetDevicePan(const std::string& device_id, int yaw, int pitch) {
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return false;
    it->second.pan_yaw = yaw;
    it->second.pan_pitch = pitch;
    it->second.has_pan = true;
    return Save();
}

bool IrStore::GetKey(const std::string& device_id, const std::string& key, IrCode* out) const {
    auto it = devices_.find(device_id);
    if (it == devices_.end()) return false;
    auto kit = it->second.keys.find(key);
    if (kit == it->second.keys.end()) return false;
    if (out != nullptr) *out = kit->second;
    return true;
}

const DeviceInfo* IrStore::FindDevice(const std::string& id) const {
    auto it = devices_.find(id);
    return it == devices_.end() ? nullptr : &it->second;
}

std::vector<DeviceInfo> IrStore::ListDevices() const {
    std::vector<DeviceInfo> out;
    out.reserve(devices_.size());
    for (const auto& kv : devices_) out.push_back(kv.second);
    return out;
}

bool IrStore::ParseJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "ParseJson: invalid json");
        return false;
    }
    std::map<std::string, DeviceInfo> parsed;
    const cJSON* arr = cJSON_GetObjectItem(root, "devices");
    if (cJSON_IsArray(arr)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, arr) {
            DeviceInfo dev;
            const cJSON* id = cJSON_GetObjectItem(item, "id");
            const cJSON* type = cJSON_GetObjectItem(item, "type");
            const cJSON* name = cJSON_GetObjectItem(item, "name");
            const cJSON* keys = cJSON_GetObjectItem(item, "keys");
            if (!cJSON_IsString(id) || id->valuestring == nullptr || id->valuestring[0] == '\0') {
                continue;
            }
            dev.id = id->valuestring;
            dev.type = cJSON_IsString(type) && type->valuestring ? type->valuestring : "";
            dev.name = cJSON_IsString(name) && name->valuestring ? name->valuestring : dev.id;
            // 云台方位预设（可选）
            const cJSON* pan = cJSON_GetObjectItem(item, "pan");
            if (cJSON_IsObject(pan)) {
                const cJSON* yaw = cJSON_GetObjectItem(pan, "yaw");
                const cJSON* pitch = cJSON_GetObjectItem(pan, "pitch");
                if (cJSON_IsNumber(yaw) && cJSON_IsNumber(pitch)) {
                    dev.pan_yaw = yaw->valueint;
                    dev.pan_pitch = pitch->valueint;
                    dev.has_pan = true;
                }
            }
            if (cJSON_IsObject(keys)) {
                const cJSON* key = nullptr;
                cJSON_ArrayForEach(key, keys) {
                    IrCode code;
                    const cJSON* protocol = cJSON_GetObjectItem(key, "protocol");
                    const cJSON* data = cJSON_GetObjectItem(key, "data");
                    const cJSON* bits = cJSON_GetObjectItem(key, "bits");
                    if (!cJSON_IsString(protocol) || protocol->valuestring == nullptr ||
                        !cJSON_IsNumber(data) || !cJSON_IsNumber(bits)) {
                        continue;
                    }
                    code.protocol = protocol->valuestring;
                    code.data = (uint64_t)data->valuedouble;
                    code.bits = (uint16_t)bits->valueint;
                    if (code.Valid()) {
                        dev.keys[key->string] = code;
                    }
                }
            }
            if (dev.Valid()) parsed[dev.id] = std::move(dev);
        }
    }
    cJSON_Delete(root);
    devices_ = std::move(parsed);
    return true;
}

bool IrStore::ImportFromJson(const std::string& json) {
    if (!ParseJson(json)) return false;
    return Save();
}

std::string IrStore::ExportToJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "devices");
    for (const auto& kv : devices_) {
        const auto& dev = kv.second;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", dev.id.c_str());
        cJSON_AddStringToObject(item, "type", dev.type.c_str());
        cJSON_AddStringToObject(item, "name", dev.name.c_str());
        if (dev.has_pan) {
            cJSON* pan = cJSON_CreateObject();
            cJSON_AddNumberToObject(pan, "yaw", dev.pan_yaw);
            cJSON_AddNumberToObject(pan, "pitch", dev.pan_pitch);
            cJSON_AddItemToObject(item, "pan", pan);
        }
        cJSON* keys = cJSON_AddObjectToObject(item, "keys");
        for (const auto& kkv : dev.keys) {
            cJSON* code = cJSON_CreateObject();
            cJSON_AddStringToObject(code, "protocol", kkv.second.protocol.c_str());
            cJSON_AddNumberToObject(code, "data", (double)kkv.second.data);
            cJSON_AddNumberToObject(code, "bits", kkv.second.bits);
            cJSON_AddItemToObject(keys, kkv.first.c_str(), code);
        }
        cJSON_AddItemToArray(arr, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) return "{}";
    std::string out(text);
    free(text);
    return out;
}

}  // namespace stackchan_ir
