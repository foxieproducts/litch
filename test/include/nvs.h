#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
using esp_err_t=int;
using nvs_handle_t=unsigned;
constexpr esp_err_t ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, ESP_FAIL=2;
constexpr unsigned NVS_READONLY=0, NVS_READWRITE=1;
namespace factory_provisioning_sdk {
inline std::array<uint8_t,8U> committed{},staged{};
inline bool exists=false, failRead=false, failWrite=false, failCommit=false;
inline unsigned writes=0U;
inline void Reset(){committed.fill(0U);staged.fill(0U);exists=failRead=failWrite=failCommit=false;writes=0U;}
}
inline esp_err_t nvs_open(const char*,unsigned mode,nvs_handle_t* h){if(mode==NVS_READONLY&&factory_provisioning_sdk::failRead)return ESP_FAIL;if(mode==NVS_READONLY&&!factory_provisioning_sdk::exists)return ESP_ERR_NVS_NOT_FOUND;*h=1U;return ESP_OK;}
inline void nvs_close(nvs_handle_t){}
inline esp_err_t nvs_get_blob(nvs_handle_t,const char*,void* p,size_t* n){if(!factory_provisioning_sdk::exists)return ESP_ERR_NVS_NOT_FOUND;if(*n!=8U)return ESP_FAIL;std::memcpy(p,factory_provisioning_sdk::committed.data(),8U);return ESP_OK;}
inline esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void* p,size_t n){++factory_provisioning_sdk::writes;if(factory_provisioning_sdk::failWrite||n!=8U)return ESP_FAIL;std::memcpy(factory_provisioning_sdk::staged.data(),p,8U);return ESP_OK;}
inline esp_err_t nvs_commit(nvs_handle_t){if(factory_provisioning_sdk::failCommit)return ESP_FAIL;factory_provisioning_sdk::committed=factory_provisioning_sdk::staged;factory_provisioning_sdk::exists=true;return ESP_OK;}
