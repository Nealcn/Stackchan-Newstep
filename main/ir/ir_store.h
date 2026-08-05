// 红外码库持久化（NVS + cJSON 序列化）
// 结构：设备（id/type/name）→ 按键 → IrCode；变更即保存；支持 JSON 导入导出
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ir_codec.h"
#include "nvs.h"

namespace stackchan_ir {

struct DeviceInfo {
    std::string id;    // 唯一标识（如 "ac1"）
    std::string type;  // 设备类型（"空调"/"电视"）
    std::string name;  // 显示名（"客厅空调"）
    std::map<std::string, IrCode> keys;  // 按键名 → 码

    bool Valid() const { return !id.empty(); }
};

class IrStore {
public:
    // 打开 NVS 并加载码库；失败时保持空库并返回 false（可继续运行）
    bool Init();

    // 设备与按键管理（操作后自动持久化）
    bool AddDevice(const DeviceInfo& dev);
    bool RemoveDevice(const std::string& id);
    bool SetKey(const std::string& device_id, const std::string& key, const IrCode& code);
    bool RemoveKey(const std::string& device_id, const std::string& key);
    bool GetKey(const std::string& device_id, const std::string& key, IrCode* out) const;
    const DeviceInfo* FindDevice(const std::string& id) const;

    std::vector<DeviceInfo> ListDevices() const;
    size_t DeviceCount() const { return devices_.size(); }

    // JSON 导入导出（导入成功覆盖全库并保存；导出用于备份/迁移）
    bool ImportFromJson(const std::string& json);
    std::string ExportToJson() const;

    // 手动持久化（SetKey 等已自动调用，一般无需手动）
    bool Save() const;

private:
    bool Load();
    bool ParseJson(const std::string& json);

    nvs_handle_t nvs_handle_;
    bool nvs_ok_ = false;
    bool loaded_ = false;
    std::map<std::string, DeviceInfo> devices_;
};

}  // namespace stackchan_ir
